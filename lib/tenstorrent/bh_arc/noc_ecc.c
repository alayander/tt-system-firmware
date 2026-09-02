/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "noc_ecc.h"
#include "noc.h"
#include "noc2axi.h"
#include "reg.h"

#include <zephyr/sys/util.h>

/* NIU_CFG_0 is the first router config register. Bit 12 gates the tile clock. */
#define NIU_CFG_0_OFF          0x100
#define NIU_CFG_0_TILE_CLK_OFF 12

/* ARC's local NOC 0 NIU. Same register block as NIU_0_A_REG_MAP_BASE_ADDR. */
#define ARC_NOC0_NIU_BASE 0x80050000
#define ARC_NOC0_X        8
#define ARC_NOC0_Y        0

static bool IsArcNoc0(uint8_t noc_x, uint8_t noc_y)
{
	return noc_x == ARC_NOC0_X && noc_y == ARC_NOC0_Y;
}

static uint64_t NiuBaseForNoc0(uint8_t noc_x, uint8_t noc_y)
{
	return NiuRegsBase(NocToPhysX(noc_x, 0), NocToPhysY(noc_y, 0), 0);
}

bool NocEccIsKnownNode(uint8_t noc_x, uint8_t noc_y)
{
	if (noc_x >= NOC_X_SIZE || noc_y >= NOC_Y_SIZE) {
		return false;
	}

	/* GDDR columns: three NOC2AXI ports per instance, filling the column. */
	if (noc_x == 0 || noc_x == 9) {
		return true;
	}

	/* Row 0 holds ARC, the two PCIE NOC2AXI nodes, and SERDES/other nodes with no NIU
	 * we know how to address.
	 */
	if (noc_y == 0) {
		return IsArcNoc0(noc_x, noc_y) || noc_x == 2 || noc_x == 11;
	}

	/* ETH row (y = 1) and the Tensix rectangle (y = 2..11), both at x 1-7 and 10-16. */
	return noc_x != 8;
}

bool NocNiuTileClockGatedLocked(uint64_t niu_base)
{
	uint32_t niu_cfg_0 = NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB, niu_base + NIU_CFG_0_OFF);

	return (niu_cfg_0 & BIT(NIU_CFG_0_TILE_CLK_OFF)) != 0;
}

bool IsSingleTileClockGated(uint8_t noc_x, uint8_t noc_y)
{
	uint64_t niu_base = NiuBaseForNoc0(noc_x, noc_y);
	bool gated;

	NocEccTlbLock();
	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, niu_base);
	gated = NocNiuTileClockGatedLocked(niu_base);
	NocEccTlbUnlock();

	return gated;
}

void NocNiuEccReadCountersLocked(uint64_t niu_base, uint32_t out[NOC_ECC_NUM_SOURCES])
{
	for (int i = 0; i < NOC_ECC_NUM_SOURCES; i++) {
		out[i] = NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB,
				       niu_base + NOC_NIU_ECC_COUNTERS_OFF + (uint32_t)i * 4);
	}
}

void NocNiuEccReadCounters(uint8_t noc_x, uint8_t noc_y, uint64_t niu_base,
			   uint32_t out[NOC_ECC_NUM_SOURCES])
{
	NocEccTlbLock();

	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y,
			niu_base + NOC_NIU_ECC_COUNTERS_OFF);
	NocNiuEccReadCountersLocked(niu_base, out);

	NocEccTlbUnlock();
}

void ArcNoc0EccReadCounters(uint32_t out[NOC_ECC_NUM_SOURCES])
{
	for (int i = 0; i < NOC_ECC_NUM_SOURCES; i++) {
		out[i] = ReadReg(ARC_NOC0_NIU_BASE + NOC_NIU_ECC_COUNTERS_OFF + (uint32_t)i * 4);
	}
}

void NocEccReadCounters(uint8_t noc_x, uint8_t noc_y, uint32_t out[NOC_ECC_NUM_SOURCES])
{
	if (IsArcNoc0(noc_x, noc_y)) {
		ArcNoc0EccReadCounters(out);
		return;
	}

	NocNiuEccReadCounters(noc_x, noc_y, NiuBaseForNoc0(noc_x, noc_y), out);
}

/* Single write to the write-only NIU ECC_CTRL. @p value must already be positioned. */
static void NocNiuEccCtrlWrite(uint8_t noc_x, uint8_t noc_y, uint64_t niu_base, uint32_t value)
{
	NocEccTlbLock();

	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, niu_base + NOC_NIU_ECC_CTRL_OFF);
	NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, niu_base + NOC_NIU_ECC_CTRL_OFF, value);

	NocEccTlbUnlock();
}

static void ArcNoc0EccCtrlWrite(uint32_t value)
{
	WriteReg(ARC_NOC0_NIU_BASE + NOC_NIU_ECC_CTRL_OFF, value);
}

static void NocEccCtrlWrite(uint8_t noc_x, uint8_t noc_y, uint32_t value)
{
	if (IsArcNoc0(noc_x, noc_y)) {
		ArcNoc0EccCtrlWrite(value);
		return;
	}

	NocNiuEccCtrlWrite(noc_x, noc_y, NiuBaseForNoc0(noc_x, noc_y), value);
}

void NocEccForce(uint8_t noc_x, uint8_t noc_y, uint8_t which)
{
	NocEccCtrlWrite(noc_x, noc_y,
			FIELD_PREP(NOC_NIU_ECC_CTRL_FORCE, which & NOC_ECC_SOURCE_MASK));
}

void NocEccClear(uint8_t noc_x, uint8_t noc_y, uint8_t which)
{
	NocEccCtrlWrite(noc_x, noc_y,
			FIELD_PREP(NOC_NIU_ECC_CTRL_CLEAR, which & NOC_ECC_SOURCE_MASK));
}
