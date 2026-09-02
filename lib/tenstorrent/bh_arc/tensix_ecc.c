/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tensix_ecc.h"
#include "noc_ecc.h"
#include "noc2axi.h"

#include <zephyr/sys/util.h>

/* ECC_CTRL[3:0] view select. */
#define ECC_CTRL_SEL_MASK GENMASK(3, 0)

/*
 * ECC_CTRL[4] holds the counters in reset and [5] holds the IRQ in reset. Both are
 * level-sensitive, not write-1-to-clear: leaving [4] set makes every counter read zero
 * forever, so a faulty part looks perfectly healthy. Always mask them out of a write.
 */
#define ECC_CTRL_STATUS_CLEAR BIT(4)
#define ECC_CTRL_IRQ_CLEAR    BIT(5)

static uint32_t TensixEccReadStatusLocked(uint8_t sel)
{
	uint32_t ctrl;

	/*
	 * Read-modify-write. ECC_CTRL[31:4] holds the per-source IRQ enables, so writing the
	 * select alone would disable every ECC interrupt in this tile.
	 */
	ctrl = NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL);
	ctrl &= ~(ECC_CTRL_SEL_MASK | ECC_CTRL_STATUS_CLEAR | ECC_CTRL_IRQ_CLEAR);
	ctrl |= FIELD_PREP(ECC_CTRL_SEL_MASK, sel);
	NOC2AXIWrite32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_CTRL, ctrl);

	return NOC2AXIRead32(NOC_ECC_RING, NOC_ECC_TLB, TENSIX_ECC_STATUS);
}

uint32_t TensixEccReadStatus(uint8_t noc_x, uint8_t noc_y, uint8_t sel)
{
	uint32_t status;

	NocEccTlbLock();
	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, TENSIX_ECC_CTRL);
	status = TensixEccReadStatusLocked(sel);
	NocEccTlbUnlock();

	return status;
}

static void DecodeL1Status(uint32_t status, uint32_t *sbe, uint32_t *dbe)
{
	*sbe = (uint32_t)TENSIX_ECC_L1_SBE_CNT(status);
	if (TENSIX_ECC_L1_SBE_OVF(status)) {
		*sbe += TENSIX_ECC_L1_SBE_MOD;
	}
	*dbe = (uint32_t)TENSIX_ECC_L1_DBE_CNT(status);
	if (TENSIX_ECC_L1_DBE_OVF(status)) {
		*dbe += TENSIX_ECC_L1_DBE_MOD;
	}
}

/* Liveness, then the sel=2 view. TLB already points at this tile. */
static bool ReadL1CountersLocked(uint32_t *sbe, uint32_t *dbe)
{
	if (TensixEccReadStatusLocked(TENSIX_ECC_SEL_LIVENESS) != TENSIX_ECC_LIVENESS_MAGIC) {
		return false;
	}

	DecodeL1Status(TensixEccReadStatusLocked(TENSIX_ECC_SEL_L1_CNT), sbe, dbe);
	return true;
}

bool TensixEccReadL1Counters(uint8_t noc_x, uint8_t noc_y, uint32_t *sbe, uint32_t *dbe)
{
	bool ok;

	NocEccTlbLock();
	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, TENSIX_ECC_CTRL);
	ok = ReadL1CountersLocked(sbe, dbe);
	NocEccTlbUnlock();

	return ok;
}

/*
 * The NOC2AXI TLB window is 2^NOC_TLB_LOG_SIZE bytes and the TLB holds addr >> 24, so
 * one program at TENSIX_ECC_CTRL also reaches the NIU block. This is what lets a tile be
 * read with one TLB write instead of three.
 */
BUILD_ASSERT((TENSIX_ECC_CTRL >> NOC_TLB_LOG_SIZE) == (NOC_NIU_REGS_BASE >> NOC_TLB_LOG_SIZE),
	     "Tensix ECC_CTRL and the NIU block must share one NOC2AXI TLB window");

void TensixEccReadTile(uint8_t noc_x, uint8_t noc_y, struct tensix_ecc_tile *out)
{
	out->l1_valid = false;
	out->l1_sbe = 0;
	out->l1_dbe = 0;

	NocEccTlbLock();
	NOC2AXITlbSetup(NOC_ECC_RING, NOC_ECC_TLB, noc_x, noc_y, TENSIX_ECC_CTRL);

	/* NIU registers stay readable with the tile clock gated; ECC_STATUS does not. */
	out->clock_gated = NocNiuTileClockGatedLocked(NOC_NIU_REGS_BASE);
	NocNiuEccReadCountersLocked(NOC_NIU_REGS_BASE, out->noc);

	if (!out->clock_gated) {
		out->l1_valid = ReadL1CountersLocked(&out->l1_sbe, &out->l1_dbe);
	}

	NocEccTlbUnlock();
}
