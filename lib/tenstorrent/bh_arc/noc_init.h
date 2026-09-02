/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NOC_INIT_H_INCLUDED
#define NOC_INIT_H_INCLUDED

#include <stdint.h>

#define NO_BAD_GDDR UINT8_MAX

/** @brief NOC2AXI TLB index reserved for ECC accesses, on both NOC rings.
 *
 * Every user takes @ref NocEccTlbLock. ECC counter and ECC_STATUS reads go over
 * @ref NOC_ECC_RING; chip-wide ROUTER_CFG_0 walks use this index on each ring in turn.
 */
#define NOC_ECC_TLB  15
#define NOC_ECC_RING 0

/** @brief Guards @ref NOC_ECC_TLB.
 *
 * Shared by every ECC NOC access: telemetry, the shell ECC commands, and the
 * ROUTER_CFG_0 walks in noc_init. Those deliberately avoid TLB 0, which the rest
 * of noc_init and reset.c program with no lock. Defined here rather than in
 * noc_ecc.c so the recovery library can use it. Innermost lock: never take the
 * ECC state lock while holding this one.
 */
void NocEccTlbLock(void);
void NocEccTlbUnlock(void);

int32_t set_tensix_enable(bool enable);

int NocInit(void);
void NocInitSingleTile(uint8_t noc0_x, uint8_t noc0_y);
void InitNocTranslation(unsigned int pcie_instance, uint16_t bad_tensix_cols, uint8_t bad_gddr,
			uint16_t skip_eth);
int InitNocTranslationFromHarvesting(void);
void ProgramNocTranslationSingleTile(uint8_t noc0_x, uint8_t noc0_y);
void ClearNocTranslation(void);
void DisableArcNocTranslation(void);
void EnableArcNocTranslation(void);
void RestoreArcNocTranslation(void);
bool IsNocTranslationEnabled(void);
void NocLogicalToPhysical(uint8_t logical_x, uint8_t logical_y, uint8_t *phys_x, uint8_t *phys_y);
void SetSingleTileClockGate(uint8_t noc0_x, uint8_t noc0_y, bool gate);

/* Returns NOC 0 coordinates of an enabled, unharvested tensix core.
 * It's guaranteed to be the same core until translation is enabled, disabled or modified.
 */
void GetEnabledTensix(uint8_t *x, uint8_t *y);

#endif
