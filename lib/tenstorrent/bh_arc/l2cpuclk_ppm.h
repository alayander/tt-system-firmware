/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_
#define LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_

#include <stdbool.h>
#include <stdint.h>

/* L2CPUCLK (PLL4) operating points, in MHz. Shared by the policy, the L2CPU power gate, and
 * tests so they cannot drift. Busy only ever uses the low/high pair.
 */
#define L2CPUCLK_PARK_MHZ      200  /* pre-release, release-safe low speed */
#define L2CPUCLK_BUSY_LOW_MHZ  1500 /* released, AICLK below threshold */
#define L2CPUCLK_BUSY_HIGH_MHZ 1750 /* released, AICLK at/above threshold */

/*
 * L2CPUCLK (PLL4) power/performance management. The target is slaved to whether an L2CPU
 * has been started (taken out of RISC reset) and to the AICLK operating point:
 *
 *   - No L2CPU started yet             -> 200 MHz
 *   - L2CPU started, AICLK >= 1340 MHz -> 1750 MHz
 *   - L2CPU started, AICLK <  1340 MHz -> 1500 MHz
 *
 * The 200 MHz park is a hardware requirement: the X280 RISC-V cores
 * must be taken out of reset at low speed so PLL4 is heldat 200 MHz until an L2CPU is released.
 * "Started" is a one-way signal (the hardware releases an L2CPU from reset at most once per
 * ASIC-reset lifetime), so once any L2CPU has started, the floor is 1500 MHz.
 *
 * The policy acts only while PLL4 is actually producing an L2CPUCLK (a nonzero rate can be
 * read). A clock-gated or unbrought-up L2CPU reads 0 and is left untouched - the decision is
 * tied to the live clock, not to the host-driven L2CPU power flag (which kmd clears on its
 * last fd close even while an L2CPU is still running).
 *
 * To keep the L2CPU clock from momentarily running too fast for the AICLK-coupled
 * operating point, the two directions are ordered relative to the AICLK step:
 *
 *   - Lowering L2CPUCLK (e.g. 1750 -> 1500 when AICLK is about to drop below the
 *     threshold) happens BEFORE AICLK decreases.
 *   - Raising L2CPUCLK (e.g. 1500 -> 1750) happens AFTER AICLK increases.
 */

/** @brief Lower L2CPUCLK if the policy target dropped. Call before AICLK is decreased. */
void DecreaseL2CPUClk(uint32_t aiclk_targ_mhz);

/** @brief Raise L2CPUCLK if the policy target rose. Call after AICLK is increased. */
void IncreaseL2CPUClk(uint32_t aiclk_targ_mhz);

/**
 * @brief Resolve the instantaneous L2CPUCLK target (MHz) for a given AICLK target and
 *        L2CPU-started state.
 */
uint32_t L2CPUClkPolicyTarget(uint32_t aiclk_targ_mhz);

/** @brief True once any L2CPU cluster has been taken out of RISC reset (a one-way signal).
 *         Used by the L2CPU power gate to pick the release-safe park vs. the running floor.
 */
bool L2CPUReleased(void);

#endif /* LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_ */
