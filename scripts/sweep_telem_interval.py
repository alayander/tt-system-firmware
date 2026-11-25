#!/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Sweep the periodic telemetry update interval downward and find the shortest
interval the firmware can sustain before the DVFS loop starts dropping ticks.

The telemetry refresh runs on the same system work queue as the 1 ms DVFS loop,
so a shorter telemetry interval means more frequent contention. When a DVFS
timer tick expires while the previous pass is still queued, the tick is coalesced
(the DVFS loop "skips a cycle") and the DVFS_COUNTER_DROPPED_TICKS counter
increments. This script finds the boundary where that starts happening.

Alongside dropped ticks, each measurement window also captures the DVFS loop's
worst-case timing for that window:
  - MAX_PERIOD_US: longest start-to-start gap between DVFS passes (target 1000 us)
  - MAX_PASS_US:   longest single DVFS pass (grows under throttling)
These show the margin shrinking before it tips into hard drops, which is most
useful under load (e.g. with tt-burnin running) where DVFS passes are longest.

For each candidate interval, and for each repeat window, the script:
  1. sets the telemetry interval and confirms it via TAG_UPDATE_TELEM_SPEED,
  2. clears the DVFS counters so the window starts fresh,
  3. dwells for a measurement window,
  4. reads dropped ticks, max period and max pass back.

Example:
    python3 sweep_telem_interval.py --asic-id 0 --dwell 15 --repeat 3 \
        --intervals 100 50 20 10 5 4 3 2 1

The telemetry interval is always restored to the default (100 ms) on exit.
"""

import argparse
import logging
import sys
import time

import pyluwen

logging.basicConfig(level=logging.INFO, format="%(message)s")
logger = logging.getLogger(__name__)

# ARC message types (see include/tenstorrent/smc_msg.h)
TT_SMC_MSG_CHARACTERISATION = 0xC6
TT_SMC_MSG_COUNTER = 0x35

# Characterization submessage ID (see include/tenstorrent/smc_msg.h)
TT_SUB_MSG_SET_TELEMETRY_UPDATE_INTERVAL = 0x5

# Counter commands / banks / indices (see include/tenstorrent/msgqueue.h)
COUNTER_CMD_GET = 0
COUNTER_CMD_CLEAR = 1
COUNTER_BANK_DVFS = 1
DVFS_COUNTER_DROPPED_TICKS = 0
DVFS_COUNTER_MAX_PERIOD_US = 1
DVFS_COUNTER_MAX_PASS_US = 2
# Period histogram bucket indices (see enum dvfs_counter_index). Each counts passes whose
# start-to-start spacing fell in that band, giving the full period distribution with no
# tracing overhead (just in-RAM atomics read out over ARC messages).
DVFS_COUNTER_PERIOD_HIST = [3, 4, 5, 6, 7]
DVFS_PERIOD_HIST_LABELS = ["<1100", "1100-1300", "1300-1500", "1500-2000", ">=2000"]
DVFS_COUNTER_COUNT = 8
# Mask covering all DVFS counters, for CLEAR.
DVFS_COUNTER_MASK_ALL = (1 << DVFS_COUNTER_COUNT) - 1

# Telemetry tags (see lib/tenstorrent/fw_common/telemetry.h)
TAG_UPDATE_TELEM_SPEED = 5

# Restored on exit; 0 would also work (firmware maps 0 -> default).
TELEM_DEFAULT_MS = 100

# Telemetry table base pointer (see app/smc/pytest/e2e_smoke.py)
TELEMETRY_DATA_REG_ADDR = 0x80030430


def read_telem(arc_chip, telem_idx):
    """Read one telemetry tag value out of the live telemetry table."""
    table_addr = arc_chip.axi_read32(TELEMETRY_DATA_REG_ADDR)
    return arc_chip.axi_read32(table_addr + telem_idx * 4)


def set_telem_interval(arc_chip, interval_ms):
    """Set the periodic telemetry update interval. Returns the firmware rc."""
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_CHARACTERISATION
            | (TT_SUB_MSG_SET_TELEMETRY_UPDATE_INTERVAL << 8),
            interval_ms,
            0,
            0,
            0,
            0,
            0,
            0,
        ]
    )
    return response[0]


def clear_dvfs_counters(arc_chip):
    """Clear all DVFS counters so the next window starts fresh."""
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_COUNTER,
            COUNTER_CMD_CLEAR | (COUNTER_BANK_DVFS << 8),
            DVFS_COUNTER_MASK_ALL,
            0,
            0,
            0,
            0,
            0,
        ]
    )
    assert response[0] == 0, f"counter CLEAR rejected (rc={response[0]})"


def read_dvfs_counter(arc_chip, index):
    """Read one DVFS counter value by index."""
    response = arc_chip.as_bh().arc_msg_buf(
        [
            TT_SMC_MSG_COUNTER,
            COUNTER_CMD_GET | (COUNTER_BANK_DVFS << 8) | (index << 16),
            0,
            0,
            0,
            0,
            0,
            0,
        ]
    )
    assert response[0] == 0, f"counter GET idx={index} rejected (rc={response[0]})"
    return response[2]


def measure_window(arc_chip, dwell_s):
    """Clear, dwell, then read back (dropped, max_period_us, max_pass_us, hist[])."""
    clear_dvfs_counters(arc_chip)
    time.sleep(dwell_s)
    dropped = read_dvfs_counter(arc_chip, DVFS_COUNTER_DROPPED_TICKS)
    max_period_us = read_dvfs_counter(arc_chip, DVFS_COUNTER_MAX_PERIOD_US)
    max_pass_us = read_dvfs_counter(arc_chip, DVFS_COUNTER_MAX_PASS_US)
    hist = [read_dvfs_counter(arc_chip, idx) for idx in DVFS_COUNTER_PERIOD_HIST]
    return dropped, max_period_us, max_pass_us, hist


def sweep(arc_chip, intervals, dwell_s, settle_s, repeat):
    """Run the sweep and return the shortest interval that stayed at 0 drops."""
    best_clean = None
    results = []

    for interval in intervals:
        rc = set_telem_interval(arc_chip, interval)
        if rc != 0:
            logger.warning(
                "  %4d ms: REJECTED by firmware (rc=%d) - below minimum? skipping",
                interval,
                rc,
            )
            results.append((interval, None, None, None, rc))
            continue

        # The new period takes effect on the next timer expiry; let it settle.
        time.sleep(settle_s)
        reported = read_telem(arc_chip, TAG_UPDATE_TELEM_SPEED)

        total_dropped = 0
        worst_period_us = 0
        worst_pass_us = 0
        total_hist = [0] * len(DVFS_COUNTER_PERIOD_HIST)
        clean = True

        for r in range(repeat):
            dropped, period_us, pass_us, hist = measure_window(arc_chip, dwell_s)
            total_dropped += dropped
            worst_period_us = max(worst_period_us, period_us)
            worst_pass_us = max(worst_pass_us, pass_us)
            total_hist = [a + b for a, b in zip(total_hist, hist)]
            if dropped != 0:
                clean = False

            if repeat > 1:
                logger.info(
                    "  %4d ms  win %d/%d: dropped=%-6d max_period=%5d us  max_pass=%5d us",
                    interval,
                    r + 1,
                    repeat,
                    dropped,
                    period_us,
                    pass_us,
                )

        status = "clean" if clean else f"DROPPED {total_dropped}"
        hist_str = "  ".join(
            f"{lbl}={cnt}" for lbl, cnt in zip(DVFS_PERIOD_HIST_LABELS, total_hist)
        )
        logger.info(
            "  %4d ms (telem reports %4d ms): dropped=%-6d "
            "max_period=%5d us  max_pass=%5d us  over %d x %.1fs  [%s]",
            interval,
            reported,
            total_dropped,
            worst_period_us,
            worst_pass_us,
            repeat,
            dwell_s,
            status,
        )
        logger.info("           period dist (us): %s", hist_str)

        results.append((interval, total_dropped, worst_period_us, worst_pass_us, rc))

        if clean:
            if best_clean is None or interval < best_clean:
                best_clean = interval

    return best_clean, results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asic-id", type=int, default=0, help="chip index (default 0)")
    parser.add_argument(
        "--dwell",
        type=float,
        default=5.0,
        help="measurement window per repeat, seconds (default 5)",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="number of dwell windows per interval (default 1)",
    )
    parser.add_argument(
        "--settle",
        type=float,
        default=0.5,
        help="settle time after changing the interval, seconds (default 0.5)",
    )
    parser.add_argument(
        "--intervals",
        type=int,
        nargs="+",
        default=[100, 50, 20, 10, 5, 4, 3, 2, 1],
        help="interval values in ms to test, high to low",
    )
    args = parser.parse_args()

    arc_chip = pyluwen.detect_chips()[args.asic_id]

    logger.info(
        "Sweeping telemetry interval on ASIC %d (dwell %.1fs x %d, settle %.1fs)",
        args.asic_id,
        args.dwell,
        args.repeat,
        args.settle,
    )
    logger.info("Intervals (ms): %s", args.intervals)

    try:
        best_clean, results = sweep(
            arc_chip, args.intervals, args.dwell, args.settle, args.repeat
        )
    finally:
        rc = set_telem_interval(arc_chip, TELEM_DEFAULT_MS)
        logger.info(
            "Restored telemetry interval to %d ms (rc=%d)", TELEM_DEFAULT_MS, rc
        )

    logger.info("")
    if best_clean is not None:
        logger.info("Shortest interval with zero dropped ticks: %d ms", best_clean)
    else:
        logger.info("No tested interval stayed at zero dropped ticks.")

    # Non-zero exit if nothing was clean, so this is usable in CI if desired.
    return 0 if best_clean is not None else 1


if __name__ == "__main__":
    sys.exit(main())
