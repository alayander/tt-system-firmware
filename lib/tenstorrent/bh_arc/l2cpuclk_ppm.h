/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_
#define LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_

#include <stdint.h>

/*
 * L2CPUCLK (PLL4) power/performance management. The target is slaved to whether an L2CPU
 * has been started (taken out of RISC reset) and to the AICLK operating point:
 *
 *   - No L2CPU started yet             -> 800 MHz
 *   - L2CPU started, AICLK >= 1340 MHz -> 1750 MHz
 *   - L2CPU started, AICLK <  1340 MHz -> 1500 MHz
 *
 * "Started" is a one-way signal (the hardware releases an L2CPU from reset at most once per
 * ASIC-reset lifetime), so 800 MHz is only the pre-first-boot baseline; once any L2CPU has
 * been started the floor is 1500 MHz until the next ASIC reset.
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
 *     threshold, or -> 800 on going idle) happens BEFORE AICLK decreases.
 *   - Raising L2CPUCLK (e.g. 1500 -> 1750) happens AFTER AICLK increases.
 *
 * The climb to 1750 additionally carries a 100 ms dwell: AICLK must stay at or
 * above the threshold continuously for that long before the clock is raised. The drop to
 * 1500 is immediate (no hold, no minimum dip), so every excursion below 1340 is covered
 * for voltage safety. The hooks reconcile against the live PLL4 rate each pass, so a rate
 * programmed outside the policy is corrected on the following pass.
 */

/** @brief Lower L2CPUCLK if the policy target dropped. Call before AICLK is decreased. */
void DecreaseL2CPUClk(uint32_t aiclk_targ_mhz);

/** @brief Raise L2CPUCLK if the policy target rose. Call after AICLK is increased. */
void IncreaseL2CPUClk(uint32_t aiclk_targ_mhz);

/**
 * @brief Resolve the instantaneous L2CPUCLK target (MHz) for a given AICLK target and
 *        L2CPU-started state. This is the hold-free target; the 100 ms raise hold is
 *        applied by the Increase/Decrease hooks, not here.
 */
uint32_t L2CPUClkPolicyTarget(uint32_t aiclk_targ_mhz);

#endif /* LIB_TENSTORRENT_BH_ARC_L2CPUCLK_PPM_H_ */
