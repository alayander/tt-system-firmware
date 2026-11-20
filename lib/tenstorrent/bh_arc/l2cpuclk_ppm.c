/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "l2cpuclk_ppm.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/dt-bindings/reset/tt-bh-reset.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(l2cpuclk_ppm, CONFIG_TT_APP_LOG_LEVEL);

/* Policy operating points, in MHz. Busy only ever uses 1500 or 1750. */
#define L2CPUCLK_IDLE_MHZ      800
#define L2CPUCLK_BUSY_LOW_MHZ  1500
#define L2CPUCLK_BUSY_HIGH_MHZ 1750

/* AICLK at/above this (MHz) lets the busy L2CPUCLK run at its high operating point. */
#define L2CPUCLK_AICLK_THRESHOLD_MHZ 1340

/*
 * PLL4's feedback divider moves in ~12.5 MHz steps,
 * so this absorbs divider rounding without churning PLL4.
 */
#define L2CPUCLK_RATE_TOLERANCE_MHZ 25

static const struct device *const pll_dev_4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll4));
static const struct device *const l2cpu_reset_dev =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(l2cpu_reset));

#define L2CPU_CLUSTER_COUNT 4

/*
 * True once the host has taken any L2CPU cluster out of RISC reset. Release is a one-way
 * hardware event (a hart leaves reset at most once per ASIC-reset lifetime, and an ASIC reset
 * restarts this firmware), so a live read of the reset lines is authoritative; there is no need
 * to cache it.
 */
static bool l2cpu_released(void)
{
	if (l2cpu_reset_dev == NULL || !device_is_ready(l2cpu_reset_dev)) {
		return false;
	}

	for (int n = 0; n < L2CPU_CLUSTER_COUNT; n++) {
		uint8_t in_reset = 1;

		if (reset_status(l2cpu_reset_dev, TT_BH_L2CPU_RISC_ID(n), &in_reset) == 0 &&
		    in_reset == 0) {
			return true;
		}
	}

	return false;
}

uint32_t L2CPUClkPolicyTarget(uint32_t aiclk_targ_mhz)
{
	if (!l2cpu_released()) {
		return L2CPUCLK_IDLE_MHZ;
	}

	return (aiclk_targ_mhz >= L2CPUCLK_AICLK_THRESHOLD_MHZ) ? L2CPUCLK_BUSY_HIGH_MHZ
							       : L2CPUCLK_BUSY_LOW_MHZ;
}

/*
 * Live PLL4 rate (MHz) for the shared L2CPUCLK VCO, or 0 if it cannot be read this pass
 * (device not ready, or the clock_control lock was busy). The hooks reconcile against
 * this actual rate rather than a cached value, so a rate programmed outside the policy
 * is corrected on the next pass instead of lingering until the target next crosses a
 * step boundary.
 */
static uint32_t l2cpuclk_actual_mhz(void)
{
	uint32_t rate = 0;

	if (pll_dev_4 == NULL || !device_is_ready(pll_dev_4) ||
	    clock_control_get_rate(pll_dev_4,
				   (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0,
				   &rate) != 0) {
		return 0;
	}

	return rate;
}

static void l2cpuclk_set(uint32_t mhz)
{
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

	sys_trace_named_event("l2cpuclk_update", mhz, 0);
}

static bool l2cpuclk_resolve(uint32_t aiclk_targ_mhz, uint32_t *target, uint32_t *actual)
{
	/*
	 * A rate of 0 means PLL4 is not producing an L2CPUCLK this pass: either it was never
	 * brought up, or the L2CPU was clock gated, so skip the pass rather than churn PLL4.
	 */
	*actual = l2cpuclk_actual_mhz();

	if (*actual == 0) {
		return false;
	}

	*target = L2CPUClkPolicyTarget(aiclk_targ_mhz);
	return true;
}

void DecreaseL2CPUClk(uint32_t aiclk_targ_mhz)
{
	uint32_t target;
	uint32_t actual;

	if (!l2cpuclk_resolve(aiclk_targ_mhz, &target, &actual)) {
		return;
	}

	/*
	 * Only the lowering direction is ordered ahead of the AICLK decrease. Compare
	 * against the live rate so a rate programmed outside the policy is pulled back
	 * down.
	 */
	if (target + L2CPUCLK_RATE_TOLERANCE_MHZ < actual) {
		l2cpuclk_set(target);
	}
}

void IncreaseL2CPUClk(uint32_t aiclk_targ_mhz)
{
	uint32_t target;
	uint32_t actual;

	if (!l2cpuclk_resolve(aiclk_targ_mhz, &target, &actual)) {
		return;
	}

	if (target > actual + L2CPUCLK_RATE_TOLERANCE_MHZ) {
		l2cpuclk_set(target);
	}
}
