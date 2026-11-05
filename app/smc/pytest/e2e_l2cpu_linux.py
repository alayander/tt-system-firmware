#!/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
End-to-end check that Linux boots on an L2CPU cluster the SMC has parked.

Exercises the host flow documented in doc/services/l2cpu_linux: fetch the
tt-bh-linux payloads with scripts/fetch_tt_bh_linux.sh, load them and make the
parked harts jump with scripts/l2cpu_boot.py, then attach the tt-bh-linux
console daemon and wait for the login prompt. The chip is reset afterwards so
the cluster is parked again for whatever runs next.
"""

import logging
import os
import pty
import re
import select
import subprocess
import sys
import time
from pathlib import Path

import pytest

logger = logging.getLogger(__name__)

TTZP = Path(__file__).resolve().parents[3]
PAYLOAD_DIR = TTZP / "build-tt-bh-linux"
FETCH_SCRIPT = TTZP / "scripts" / "fetch_tt_bh_linux.sh"
BOOT_SCRIPT = TTZP / "scripts" / "l2cpu_boot.py"
DAEMON = PAYLOAD_DIR / "tt-bh-linux"
ROOTFS = PAYLOAD_DIR / "rootfs.ext4"

NUM_CLUSTERS = 4
ALL_HARTS = 0xF

# Console milestones, in the order they appear; the last one is the goal
MILESTONES = [b"OpenSBI v", b"Linux version", b"VFS: Mounted root", b"login:"]
# Seen locally: ~6 s from jump to login prompt. Allow for a slow disk.
LOGIN_TIMEOUT_S = 180


def _run(cmd, timeout):
    logger.info("$ %s", " ".join(str(c) for c in cmd))
    result = subprocess.run(
        [str(c) for c in cmd], capture_output=True, text=True, timeout=timeout
    )
    for line in (result.stdout + result.stderr).splitlines():
        logger.info("  %s", line)
    return result


def _tt_smi_reset():
    result = _run(["tt-smi", "-r"], timeout=180)
    assert result.returncode == 0, "tt-smi -r failed"


def _parked_mask(asic_id, cluster):
    result = _run(
        [sys.executable, BOOT_SCRIPT, "-d", asic_id, "-l", cluster, "status"],
        timeout=60,
    )
    assert result.returncode == 0, "l2cpu_boot.py status failed"
    match = re.search(r"parked in resident stub: 0x([0-9a-f]+)", result.stdout)
    assert match, f"unexpected status output: {result.stdout!r}"
    return int(match.group(1), 16)


def _find_parked_cluster(asic_id):
    for cluster in range(NUM_CLUSTERS):
        if _parked_mask(asic_id, cluster) == ALL_HARTS:
            return cluster
    return None


def _wait_for_login(asic_id, cluster):
    """
    Run the console daemon on a pty (it drives the terminal) and collect its
    output until the login prompt appears. Returns the console text.
    """
    master, slave = pty.openpty()
    daemon = subprocess.Popen(
        [
            str(DAEMON),
            "--ttdevice",
            str(asic_id),
            "--l2cpu",
            str(cluster),
            "--disk",
            str(ROOTFS),
        ],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)

    buf = b""
    seen = []
    start = time.monotonic()
    try:
        while time.monotonic() - start < LOGIN_TIMEOUT_S:
            ready, _, _ = select.select([master], [], [], 1.0)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if not data:
                    break
                buf += data
                for marker in MILESTONES:
                    if marker in buf and marker not in seen:
                        seen.append(marker)
                        logger.info(
                            "%6.1f s: %s", time.monotonic() - start, marker.decode()
                        )
                if MILESTONES[-1] in buf:
                    break
            if daemon.poll() is not None:
                logger.error("console daemon exited with %d", daemon.returncode)
                break
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
        os.close(master)

    return buf.decode(errors="replace"), seen


@pytest.fixture(scope="module")
def payloads():
    """Fetch the tt-bh-linux release payloads and build the console daemon."""
    assert FETCH_SCRIPT.is_file(), f"{FETCH_SCRIPT} missing"
    result = _run([FETCH_SCRIPT, "-o", PAYLOAD_DIR], timeout=1800)
    assert result.returncode == 0, "fetch_tt_bh_linux.sh failed"
    for f in (DAEMON, ROOTFS):
        assert f.is_file(), f"{f} missing after fetch"
    return PAYLOAD_DIR


@pytest.mark.stress
def test_l2cpu_linux_boot(arc_chip_dut, asic_id, payloads):
    """
    Boot Linux on the parked cluster and wait for its login prompt.
    """
    test_name = "L2CPU Linux boot test"
    try:
        cluster = _find_parked_cluster(asic_id)
        if cluster is None:
            # Something earlier may have jumped it; a chip reset re-parks it
            _tt_smi_reset()
            cluster = _find_parked_cluster(asic_id)
        assert cluster is not None, "no L2CPU cluster parked in the resident stub"
        logger.info("Booting Linux on L2CPU%d", cluster)

        result = _run(
            [sys.executable, BOOT_SCRIPT, "-d", asic_id, "-l", cluster, "boot"],
            timeout=300,
        )
        assert result.returncode == 0, "l2cpu_boot.py boot failed"
        assert "jumped to" in result.stdout, "harts did not leave the stub"
        assert _parked_mask(asic_id, cluster) == 0, "harts still reported parked"

        console, seen = _wait_for_login(asic_id, cluster)
        if MILESTONES[-1] not in seen:
            logger.error("console output:\n%s", console)
        assert MILESTONES[-1] in seen, (
            f"no login prompt within {LOGIN_TIMEOUT_S} s; reached "
            f"{[m.decode() for m in seen]}"
        )
        logger.info(f"{test_name} completed. Failed 0/1 times.")
    finally:
        # Leave the card parked again for whatever runs next
        _tt_smi_reset()
