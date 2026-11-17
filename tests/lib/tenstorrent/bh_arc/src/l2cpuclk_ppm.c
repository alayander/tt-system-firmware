/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "l2cpuclk_ppm.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/dt-bindings/reset/tt-bh-reset.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

#define L2CPUCLK_IDLE_MHZ      800
#define L2CPUCLK_BUSY_LOW_MHZ  1500
#define L2CPUCLK_BUSY_HIGH_MHZ 1750
#define AICLK_THRESHOLD_MHZ    1340

/* AICLK targets comfortably on either side of the threshold. */
#define AICLK_HIGH_MHZ 1400
#define AICLK_LOW_MHZ  1200

/* Must match L2CPUCLK_RAISE_HOLD_MS in the policy. */
#define RAISE_HOLD_MS 100

/* Must match L2CPU_CLUSTER_COUNT in the policy. */
#define L2CPU_CLUSTER_COUNT 4

static const struct device *pll4;
static const struct device *l2cpu_reset_dev;

static uint32_t get_l2cpuclk(void)
{
	uint32_t rate = 0;

	zassert_ok(clock_control_get_rate(
			   pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
			   &rate),
		   "Failed to read L2CPUCLK rate");
	return rate;
}

/* Simulate a rate programmed outside the policy (e.g. a host or boot-script PLL4 write)
 * by driving the clock directly, bypassing the policy hooks.
 */
static void inject_external_rate(uint32_t mhz)
{
	zassert_ok(clock_control_set_rate(
			   pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
			   (clock_control_subsys_rate_t)mhz),
		   "Failed to inject external L2CPUCLK rate");
}

/* Drive the "L2CPU started" signal the policy gates on: deassert (released) or assert
 * (held in reset) every cluster's RISC reset line. In hardware release is one-way; the
 * emul reset controller lets us clear it again so tests can exercise both branches.
 */
static void set_l2cpu_released(bool released)
{
	for (int n = 0; n < L2CPU_CLUSTER_COUNT; n++) {
		if (released) {
			zassert_ok(reset_line_deassert(l2cpu_reset_dev,
						       TT_BH_L2CPU_RISC_ID(n)),
				   "Failed to release L2CPU RISC reset");
		} else {
			zassert_ok(reset_line_assert(l2cpu_reset_dev, TT_BH_L2CPU_RISC_ID(n)),
				   "Failed to hold L2CPU RISC reset");
		}
	}
}

/* Drive the L2CPU power domain via the power-setting message (the only path that
 * updates the state the policy gates PLL4 access on). Tensix/MRISC are held enabled so
 * toggling L2CPU has no side effect on them.
 */
static void set_l2cpu_power(bool on)
{
	union request req = {0};
	struct response rsp = {0};

	req.power_setting.command_code = TT_SMC_MSG_POWER_SETTING;
	req.power_setting.power_flags_valid = 4; /* AICLK, MRISC, Tensix, L2CPU */
	req.power_setting.power_flags_bitfield.max_ai_clk = 0;
	req.power_setting.power_flags_bitfield.mrisc_phy_power = 1;
	req.power_setting.power_flags_bitfield.tensix_enable = 1;
	req.power_setting.power_flags_bitfield.l2cpu_enable = on ? 1 : 0;
	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);
	zexpect_equal(rsp.data[0], 0);
}

/* Apply a full DVFS-style bracket: lower-before then raise-after the AICLK step. */
static void apply_policy(uint32_t aiclk_targ_mhz)
{
	DecreaseL2CPUClk(aiclk_targ_mhz);
	IncreaseL2CPUClk(aiclk_targ_mhz);
}

/* Drive the clock to the busy-high operating point, satisfying the 100 ms raise hold.
 * The caller must already have started an L2CPU (set_l2cpu_released(true)).
 */
static void settle_busy_high(uint32_t aiclk_targ_mhz)
{
	apply_policy(aiclk_targ_mhz);       /* begins the AICLK>=threshold streak */
	k_sleep(K_MSEC(RAISE_HOLD_MS));     /* let the raise hold elapse */
	apply_policy(aiclk_targ_mhz);       /* raise now completes */
}

static void *l2cpuclk_setup(void)
{
	pll4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll4));
	zassert_not_null(pll4, "pll4 device missing");
	zassert_true(device_is_ready(pll4), "pll4 device not ready");

	l2cpu_reset_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(l2cpu_reset));
	zassert_not_null(l2cpu_reset_dev, "l2cpu_reset device missing");
	zassert_true(device_is_ready(l2cpu_reset_dev), "l2cpu_reset device not ready");
	return NULL;
}

/* Start every test powered on, no L2CPU started yet, settled at the 800 MHz baseline. */
static void l2cpuclk_before(void *fixture)
{
	ARG_UNUSED(fixture);
	set_l2cpu_power(true);
	set_l2cpu_released(false);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_IDLE_MHZ, "Expected idle 800 MHz at start");
}

ZTEST(l2cpuclk_ppm, test_policy_target_resolution)
{
	/* L2CPUClkPolicyTarget is the instantaneous (hold-free) resolver. */
	set_l2cpu_released(false);
	zassert_equal(L2CPUClkPolicyTarget(AICLK_HIGH_MHZ), L2CPUCLK_IDLE_MHZ,
		      "Not started must resolve to 800 MHz regardless of AICLK");

	set_l2cpu_released(true);
	zassert_equal(L2CPUClkPolicyTarget(AICLK_HIGH_MHZ), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Started + AICLK>=1340 must resolve to 1750 MHz");
	zassert_equal(L2CPUClkPolicyTarget(AICLK_THRESHOLD_MHZ), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Started + AICLK==1340 must resolve to 1750 MHz");
	zassert_equal(L2CPUClkPolicyTarget(AICLK_THRESHOLD_MHZ - 1), L2CPUCLK_BUSY_LOW_MHZ,
		      "Started + AICLK<1340 must resolve to 1500 MHz");
	zassert_equal(L2CPUClkPolicyTarget(AICLK_LOW_MHZ), L2CPUCLK_BUSY_LOW_MHZ,
		      "Started + low AICLK must resolve to 1500 MHz");
}

/* Until an L2CPU is started, the clock stays at the 800 MHz baseline even if AICLK is high. */
ZTEST(l2cpuclk_ppm, test_not_started_is_idle)
{
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_IDLE_MHZ,
		      "No L2CPU started -> 800 MHz regardless of AICLK");
}

/* Starting an L2CPU raises the floor to 1500 immediately - the 1500 floor is not
 * hold-gated (only the climb to 1750 is).
 */
ZTEST(l2cpuclk_ppm, test_start_raises_floor_to_1500)
{
	set_l2cpu_released(true);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Starting an L2CPU raises the floor to 1500 MHz immediately");
}

/* The climb to 1750 is gated by the 100 ms raise hold: it holds at 1500 first. */
ZTEST(l2cpuclk_ppm, test_started_rise_to_high)
{
	set_l2cpu_released(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Started at high AICLK holds at 1500 MHz until the raise hold elapses");

	k_sleep(K_MSEC(RAISE_HOLD_MS));
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "After the 100 ms hold, started at high AICLK reaches 1750 MHz");
}

ZTEST(l2cpuclk_ppm, test_started_drop_below_threshold)
{
	set_l2cpu_released(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	/* AICLK stepping below the threshold must bring L2CPUCLK down to 1500 MHz
	 * immediately - no hold, no minimum dip - in a single pass.
	 */
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Dropping AICLK below 1340 should lower L2CPUCLK to 1500 MHz immediately");
}

/* Raising across the threshold is held at 1500 until AICLK has stayed high for 100 ms. */
ZTEST(l2cpuclk_ppm, test_started_rise_above_threshold)
{
	set_l2cpu_released(true);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Precondition: 1500 MHz");

	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "AICLK just crossed 1340: must hold at 1500 MHz during the raise hold");

	k_sleep(K_MSEC(RAISE_HOLD_MS));
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "After the 100 ms hold, AICLK above 1340 should raise L2CPUCLK to 1750 MHz");
}

/* Release is one-way: once latched, clearing the reset line (only possible in emul) must
 * NOT return the policy to the 800 MHz pre-boot baseline - the floor stays at 1500 MHz.
 */
ZTEST(l2cpuclk_ppm, test_unstarted_returns_to_idle)
{
	set_l2cpu_released(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	/* With the L2CPU back in reset, the policy must fall to the 800 MHz idle baseline. */
	set_l2cpu_released(false);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_IDLE_MHZ,
		      "Unstarted L2CPU must return to 800 MHz idle");
}

/* The lower-before hook must only lower; the raise is the after hook's job - even once
 * the raise hold has already elapsed.
 */
ZTEST(l2cpuclk_ppm, test_decrease_hook_never_raises)
{
	set_l2cpu_released(true);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Precondition: 1500 MHz");

	/* Start and satisfy the raise hold so the target genuinely wants 1750. */
	apply_policy(AICLK_HIGH_MHZ);
	k_sleep(K_MSEC(RAISE_HOLD_MS));

	/* The decrease-before hook alone must still not raise. */
	DecreaseL2CPUClk(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Decrease-before hook must not raise L2CPUCLK");

	IncreaseL2CPUClk(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Increase-after hook should complete the raise to 1750 MHz");
}

/* The raise-after hook must only raise; a pending drop is handled by the before hook. */
ZTEST(l2cpuclk_ppm, test_increase_hook_never_lowers)
{
	set_l2cpu_released(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	/* Low AICLK target wants 1500, but the increase-after hook alone must not lower. */
	IncreaseL2CPUClk(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Increase-after hook must not lower L2CPUCLK");

	DecreaseL2CPUClk(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Decrease-before hook should complete the drop to 1500 MHz");
}

/* A clock-gated L2CPU reads a rate of 0, and the policy must leave such a cluster untouched
 * (and resume once the clock is running again). The gate is the live rate, not the host power
 * flag: clock gating via bh_set_l2cpu_enable() is what drops the rate to 0.
 */
ZTEST(l2cpuclk_ppm, test_gated_clock_is_noop)
{
	/* Establish a known 1750 MHz point, then clock gate the cluster off. */
	set_l2cpu_released(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	set_l2cpu_power(false);
	zassert_equal(get_l2cpuclk(), 0, "A gated-off L2CPUCLK must read 0 MHz");

	/* Both a not-started pass (would lower) and a high-AICLK pass (would raise) must
	 * no-op while the clock reads 0 - the policy must not retune a gated cluster.
	 */
	set_l2cpu_released(false);
	apply_policy(AICLK_LOW_MHZ);
	set_l2cpu_released(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), 0, "Policy must not retune a gated (0 MHz) L2CPUCLK");

	/* Un-gate the clock and confirm the policy resumes driving it (a fresh hold is
	 * required - the gap cleared the streak).
	 */
	set_l2cpu_power(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Policy should resume once the clock is running again");
}

/* A rate programmed outside the policy (host/boot-script PLL4 write) that leaves the
 * clock too high must be reconciled on the next pass - even when the AICLK target does
 * not move across a threshold. This is the level- (not edge-) triggered behaviour.
 */
ZTEST(l2cpuclk_ppm, test_external_overwrite_pulled_down)
{
	set_l2cpu_released(true);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Precondition: 1500 MHz");

	/* External writer forces 1750 while AICLK stays below the threshold. */
	inject_external_rate(L2CPUCLK_BUSY_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Injected rate should be 1750 MHz");

	/* Re-running the policy at the SAME (unchanged, below-threshold) AICLK target must
	 * still pull the clock back to the policy target.
	 */
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Policy must pull an externally-raised rate back to 1500 MHz");
}

/* The reverse: an external writer leaving the clock too low must be raised back to the
 * policy target. AICLK stayed above the threshold throughout (the hold was already met),
 * so the correction is immediate - the hold defends against AICLK jitter, not against an
 * external clock write.
 */
ZTEST(l2cpuclk_ppm, test_external_underwrite_pulled_up)
{
	set_l2cpu_released(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	inject_external_rate(L2CPUCLK_IDLE_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_IDLE_MHZ, "Injected rate should be 800 MHz");

	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Policy must raise an externally-lowered rate back to 1750 MHz");
}

/* A single sub-threshold dip must drop immediately AND restart the raise hold, so the
 * climb back to 1750 re-earns the full 100 ms.
 */
ZTEST(l2cpuclk_ppm, test_raise_hold_reset_by_dip)
{
	set_l2cpu_released(true);
	settle_busy_high(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	/* One dip below the threshold: immediate drop. */
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Single dip should drop to 1500 MHz");

	/* AICLK back above the threshold must NOT jump straight to 1750 - fresh hold. */
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Re-raise after a dip must re-earn the full hold, not jump to 1750");

	k_sleep(K_MSEC(RAISE_HOLD_MS));
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "After a fresh 100 ms hold the clock reaches 1750 MHz");
}

/* Hold progress is not cumulative across a dip: time accrued before a dip is discarded,
 * even if the total elapsed time since the first high pass exceeds the hold.
 */
ZTEST(l2cpuclk_ppm, test_raise_hold_not_cumulative_across_dip)
{
	set_l2cpu_released(true);

	/* Accrue most of the hold at high AICLK. */
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Holding at 1500 MHz");
	k_sleep(K_MSEC(80));

	/* Dip resets the streak. */
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Still 1500 MHz after the dip");

	/* More than the hold has now elapsed since the FIRST high pass (80 + 30 > 100), but
	 * only 30 ms since the dip, so the clock must still hold at 1500.
	 */
	k_sleep(K_MSEC(30));
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Pre-dip hold progress must not count toward the post-dip hold");

	k_sleep(K_MSEC(RAISE_HOLD_MS));
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Clock reaches 1750 MHz once a full post-dip hold elapses");
}

/* Leave shared state clean (L2CPU powered, not started) for later suites. */
static void l2cpuclk_after(void *fixture)
{
	ARG_UNUSED(fixture);
	set_l2cpu_released(false);
	set_l2cpu_power(true);
}

ZTEST_SUITE(l2cpuclk_ppm, NULL, l2cpuclk_setup, l2cpuclk_before, l2cpuclk_after, NULL);
