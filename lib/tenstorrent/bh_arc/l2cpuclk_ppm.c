/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aiclk_ppm.h"
#include "l2cpuclk_ppm.h"

#include <tenstorrent/bh_power.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(l2cpuclk_ppm, CONFIG_TT_APP_LOG_LEVEL);

/* Policy operating points, in MHz. Busy only ever uses 1500 or 1750. */
#define L2CPUCLK_IDLE_MHZ      800
#define L2CPUCLK_BUSY_LOW_MHZ  1500
#define L2CPUCLK_BUSY_HIGH_MHZ 1750

/* AICLK at/above this (MHz) lets the busy L2CPUCLK run at its high operating point. */
#define L2CPUCLK_AICLK_THRESHOLD_MHZ 1340

static const struct device *const pll_dev_4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll4));

/*
 * Last frequency (MHz) PLL4 programmed to. 0 means "not yet known"; the first
 * update seeds it from hardware so the initial step moves in the correct direction.
 */
static uint32_t l2cpuclk_curr_mhz;

/*
 * The policy only drives PLL4 while the L2CPU power domain is on. With the clusters
 * powered down the external postdivs are clock gated, so retuning the VCO has no effect;
 * skip it rather than churn PLL4 on every DVFS pass.
 */
static bool l2cpuclk_powered(void)
{
	bool enabled = false;

	if (bh_power_state_get(BH_POWER_DOMAIN_L2CPU, &enabled) != 0 || !enabled) {
		l2cpuclk_curr_mhz = 0;
		return false;
	}

	return true;
}

uint32_t L2CPUClkPolicyTarget(uint32_t aiclk_targ_mhz)
{
	if (!aiclk_get_busy()) {
		return L2CPUCLK_IDLE_MHZ;
	}

	return (aiclk_targ_mhz >= L2CPUCLK_AICLK_THRESHOLD_MHZ) ? L2CPUCLK_BUSY_HIGH_MHZ
							       : L2CPUCLK_BUSY_LOW_MHZ;
}

/* Seed l2cpuclk_curr_mhz from the current PLL4 rate the first time we run. */
static void l2cpuclk_ensure_seeded(void)
{
	if (l2cpuclk_curr_mhz != 0) {
		return;
	}

	uint32_t rate = 0;

	if (pll_dev_4 != NULL && device_is_ready(pll_dev_4) &&
	    clock_control_get_rate(pll_dev_4,
				   (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
				   &rate) == 0 &&
	    rate != 0) {
		l2cpuclk_curr_mhz = rate;
	} else {
		l2cpuclk_curr_mhz = L2CPUCLK_IDLE_MHZ;
	}
}

static void l2cpuclk_set(uint32_t mhz)
{
	if (mhz == l2cpuclk_curr_mhz) {
		return;
	}

	if (pll_dev_4 == NULL || !device_is_ready(pll_dev_4)) {
		return;
	}

	/* All four L2CPUCLK outputs share PLL4's VCO, so a single set_rate retunes the
	 * cluster together.
	 */
	int ret = clock_control_set_rate(
		pll_dev_4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
		(clock_control_subsys_rate_t)mhz);

	if (ret != 0) {
		LOG_ERR("Failed to set L2CPUCLK to %u MHz: %d", mhz, ret);
		return;
	}

	l2cpuclk_curr_mhz = mhz;
	sys_trace_named_event("l2cpuclk_update", mhz, 0);
}

void DecreaseL2CPUClk(uint32_t aiclk_targ_mhz)
{
	if (!l2cpuclk_powered()) {
		return;
	}

	l2cpuclk_ensure_seeded();

	uint32_t target = L2CPUClkPolicyTarget(aiclk_targ_mhz);

	/* Only the lowering direction is ordered ahead of the AICLK decrease. */
	if (target < l2cpuclk_curr_mhz) {
		l2cpuclk_set(target);
	}
}

void IncreaseL2CPUClk(uint32_t aiclk_targ_mhz)
{
	if (!l2cpuclk_powered()) {
		return;
	}

	l2cpuclk_ensure_seeded();

	uint32_t target = L2CPUClkPolicyTarget(aiclk_targ_mhz);

	/* Raising follows the AICLK increase, so it may track AICLK immediately. */
	if (target > l2cpuclk_curr_mhz) {
		l2cpuclk_set(target);
	}
}
