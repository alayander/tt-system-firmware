/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_
#define LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_

#include <stdint.h>

/*
 * L2CPUCLK (PLL4) power/performance management. The target is slaved to AICLK and
 * the busy/idle state:
 *
 *   - Idle                         -> 800 MHz
 *   - Busy and AICLK >= 1340 MHz   -> 1750 MHz
 *   - Busy and AICLK <  1340 MHz   -> 1500 MHz
 *
 * To keep the L2CPU clock from momentarily running too fast for the AICLK-coupled
 * operating point, the two directions are ordered relative to the AICLK step:
 *
 *   - Lowering L2CPUCLK (e.g. 1750 -> 1500 when AICLK is about to drop below the
 *     threshold, or -> 800 on going idle) happens BEFORE AICLK decreases.
 *   - Raising L2CPUCLK (e.g. 1500 -> 1750) happens AFTER AICLK increases, so it
 *     may follow AICLK immediately.
 */

/** @brief Lower L2CPUCLK if the policy target dropped. Call before AICLK is decreased. */
void DecreaseL2CPUClk(uint32_t aiclk_targ_mhz);

/** @brief Raise L2CPUCLK if the policy target rose. Call after AICLK is increased. */
void IncreaseL2CPUClk(uint32_t aiclk_targ_mhz);

/** @brief Resolve the L2CPUCLK target (MHz) for a given AICLK target and busy state. */
uint32_t L2CPUClkPolicyTarget(uint32_t aiclk_targ_mhz);

#endif /* LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_ */
