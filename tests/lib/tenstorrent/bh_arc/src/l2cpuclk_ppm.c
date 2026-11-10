/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aiclk_ppm.h"
#include "l2cpuclk_ppm.h"

#include <tenstorrent/bh_power.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

#define L2CPUCLK_IDLE_MHZ      800
#define L2CPUCLK_BUSY_LOW_MHZ  1500
#define L2CPUCLK_BUSY_HIGH_MHZ 1750
#define AICLK_THRESHOLD_MHZ    1340

/* AICLK targets comfortably on either side of the threshold. */
#define AICLK_HIGH_MHZ 1400
#define AICLK_LOW_MHZ  1200

static const struct device *pll4;

static uint32_t get_l2cpuclk(void)
{
	uint32_t rate = 0;

	zassert_ok(clock_control_get_rate(
			   pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
			   &rate),
		   "Failed to read L2CPUCLK rate");
	return rate;
}

static void set_busy(bool busy)
{
	union request req = {0};
	struct response rsp = {0};

	req.aiclk_set_speed.command_code =
		busy ? TT_SMC_MSG_AICLK_GO_BUSY : TT_SMC_MSG_AICLK_GO_LONG_IDLE;
	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);
	zexpect_equal(rsp.data[0], 0);
}

/* Drive the L2CPU power domain via the power-setting message (the only path that
 * updates the state the policy gates on). Tensix/MRISC are held enabled so toggling
 * L2CPU has no side effect on them.
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

static bool l2cpu_powered(void)
{
	bool state = false;

	zassert_ok(bh_power_state_get(BH_POWER_DOMAIN_L2CPU, &state),
		   "Failed to read L2CPU power state");
	return state;
}

/* Apply a full DVFS-style bracket: lower-before then raise-after the AICLK step. */
static void apply_policy(uint32_t aiclk_targ_mhz)
{
	DecreaseL2CPUClk(aiclk_targ_mhz);
	IncreaseL2CPUClk(aiclk_targ_mhz);
}

static void *l2cpuclk_setup(void)
{
	pll4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll4));
	zassert_not_null(pll4, "pll4 device missing");
	zassert_true(device_is_ready(pll4), "pll4 device not ready");
	return NULL;
}

/* Start every test with L2CPU powered on and settled idle at 800 MHz. */
static void l2cpuclk_before(void *fixture)
{
	ARG_UNUSED(fixture);
	set_l2cpu_power(true);
	set_busy(false);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_IDLE_MHZ, "Expected idle 800 MHz at start");
}

ZTEST(l2cpuclk_ppm, test_policy_target_resolution)
{
	set_busy(false);
	zassert_equal(L2CPUClkPolicyTarget(AICLK_HIGH_MHZ), L2CPUCLK_IDLE_MHZ,
		      "Idle must resolve to 800 MHz regardless of AICLK");

	set_busy(true);
	zassert_equal(L2CPUClkPolicyTarget(AICLK_HIGH_MHZ), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Busy + AICLK>=1340 must resolve to 1750 MHz");
	zassert_equal(L2CPUClkPolicyTarget(AICLK_THRESHOLD_MHZ), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Busy + AICLK==1340 must resolve to 1750 MHz");
	zassert_equal(L2CPUClkPolicyTarget(AICLK_THRESHOLD_MHZ - 1), L2CPUCLK_BUSY_LOW_MHZ,
		      "Busy + AICLK<1340 must resolve to 1500 MHz");
	zassert_equal(L2CPUClkPolicyTarget(AICLK_LOW_MHZ), L2CPUCLK_BUSY_LOW_MHZ,
		      "Busy + low AICLK must resolve to 1500 MHz");
}

ZTEST(l2cpuclk_ppm, test_idle_to_busy_high)
{
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Busy at high AICLK should raise L2CPUCLK to 1750 MHz");
}

ZTEST(l2cpuclk_ppm, test_busy_drop_below_threshold)
{
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	/* AICLK stepping below the threshold must bring L2CPUCLK down to 1500 MHz. */
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Dropping AICLK below 1340 should lower L2CPUCLK to 1500 MHz");
}

ZTEST(l2cpuclk_ppm, test_busy_rise_above_threshold)
{
	set_busy(true);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Precondition: 1500 MHz");

	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Raising AICLK above 1340 should raise L2CPUCLK to 1750 MHz");
}

ZTEST(l2cpuclk_ppm, test_busy_to_idle)
{
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	set_busy(false);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_IDLE_MHZ,
		      "Going idle should drop L2CPUCLK to 800 MHz");
}

/* The lower-before hook must only lower; a pending raise waits for the after hook. */
ZTEST(l2cpuclk_ppm, test_decrease_hook_never_raises)
{
	set_busy(true);
	apply_policy(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ, "Precondition: 1500 MHz");

	/* High AICLK target wants 1750, but the decrease-before hook alone must not raise. */
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
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	/* Low AICLK target wants 1500, but the increase-after hook alone must not lower. */
	IncreaseL2CPUClk(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Increase-after hook must not lower L2CPUCLK");

	DecreaseL2CPUClk(AICLK_LOW_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_LOW_MHZ,
		      "Decrease-before hook should complete the drop to 1500 MHz");
}

/* The policy must leave the clock untouched while the L2CPU domain is powered down, and
 * resume driving it once the domain is powered back up.
 */
ZTEST(l2cpuclk_ppm, test_powered_off_is_noop)
{
	/* Establish a known 1750 MHz point while powered, then power the domain down. */
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ, "Precondition: 1750 MHz");

	set_l2cpu_power(false);
	zassert_false(l2cpu_powered(), "L2CPU domain should be powered down");

	uint32_t rate_before = get_l2cpuclk();

	/* Both busy->idle (would lower) and a high-AICLK pass (would raise) must no-op
	 * while powered down - the policy must not retune a gated cluster.
	 */
	set_busy(false);
	apply_policy(AICLK_LOW_MHZ);
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), rate_before,
		      "Policy must not retune L2CPUCLK while powered down");

	/* Re-enable the domain and confirm the policy resumes driving it. */
	set_l2cpu_power(true);
	set_busy(true);
	apply_policy(AICLK_HIGH_MHZ);
	zassert_equal(get_l2cpuclk(), L2CPUCLK_BUSY_HIGH_MHZ,
		      "Policy should resume once the domain is powered back up");
}

/* Leave shared state clean (L2CPU powered, AICLK idle) for later suites. */
static void l2cpuclk_after(void *fixture)
{
	ARG_UNUSED(fixture);
	set_busy(false);
	set_l2cpu_power(true);
}

ZTEST_SUITE(l2cpuclk_ppm, NULL, l2cpuclk_setup, l2cpuclk_before, l2cpuclk_after, NULL);
