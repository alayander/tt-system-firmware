/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSIX_ECC_H
#define TENSIX_ECC_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "noc_ecc.h"

/* Per-Tensix RISCV debug registers. One instance per tile, shared by all five RISC cores. */
#define TENSIX_ECC_CTRL   0xFFB121D0
#define TENSIX_ECC_STATUS 0xFFB121D4

/*
 * ECC_STATUS is a windowed register: ECC_CTRL[3:0] selects which view it returns.
 * Any select other than 1-6 returns TENSIX_ECC_LIVENESS_MAGIC.
 */
#define TENSIX_ECC_SEL_L1_CNT   2U   /* L1 SBE/DBE counters */
#define TENSIX_ECC_SEL_LIVENESS 0xFU /* not a view - returns the magic value */

#define TENSIX_ECC_LIVENESS_MAGIC 0xDEADBEEFU

/* sel=2. Widths are Blackhole-specific: SBE 8 bits, DBE 6 bits.
 * The overflow bit is sticky and records one wrap; the counter keeps going.
 */
#define TENSIX_ECC_L1_DBE_CNT(s) FIELD_GET(GENMASK(5, 0), (s))
#define TENSIX_ECC_L1_DBE_OVF(s) FIELD_GET(BIT(6), (s))
#define TENSIX_ECC_L1_DBE_MOD    (1U << 6)
#define TENSIX_ECC_L1_SBE_CNT(s) FIELD_GET(GENMASK(14, 7), (s))
#define TENSIX_ECC_L1_SBE_OVF(s) FIELD_GET(BIT(15), (s))
#define TENSIX_ECC_L1_SBE_MOD    (1U << 8)

/**
 * @brief Read one view of a Tensix tile's ECC_STATUS register.
 *
 * Writes @p sel into ECC_CTRL[3:0] and reads back ECC_STATUS. The write is a
 * read-modify-write that preserves ECC_CTRL[31:4] (the IRQ enables) and never sets the
 * level-sensitive clear bits [4] and [5], so this is non-destructive to other observers.
 *
 * The caller must ensure Tensix is powered - a NOC read to an unclocked tile can hang the
 * ARC. The caller should also confirm liveness (@ref TENSIX_ECC_SEL_LIVENESS returns
 * @ref TENSIX_ECC_LIVENESS_MAGIC) before trusting a zero from any other view, since a
 * healthy tile and a failed read both read as zero.
 *
 * @param noc_x NOC 0 X coordinate of the Tensix tile.
 * @param noc_y NOC 0 Y coordinate of the Tensix tile.
 * @param sel   Which view to select, one of TENSIX_ECC_SEL_*.
 *
 * @return The raw ECC_STATUS value for that view.
 */
uint32_t TensixEccReadStatus(uint8_t noc_x, uint8_t noc_y, uint8_t sel);

/**
 * @brief Read a tile's L1 SBE and DBE counters.
 *
 * Confirms ECC_STATUS liveness, then returns the sel=2 counts. A set overflow
 * bit adds one modulus (@ref TENSIX_ECC_L1_SBE_MOD or @ref TENSIX_ECC_L1_DBE_MOD).
 * The bit stays set, so a second wrap is not counted.
 *
 * @param noc_x NOC 0 X coordinate of the Tensix tile. Must be clocked.
 * @param noc_y NOC 0 Y coordinate of the Tensix tile.
 * @param sbe   Receives the L1 SBE count, including one overflow when the flag is set.
 * @param dbe   Receives the L1 DBE count, including one overflow when the flag is set.
 *
 * @return true if liveness matched and @p sbe / @p dbe were written.
 */
bool TensixEccReadL1Counters(uint8_t noc_x, uint8_t noc_y, uint32_t *sbe, uint32_t *dbe);

/** @brief Everything the telemetry walk wants from one Tensix tile. */
struct tensix_ecc_tile {
	/** NOC 0 NIU ECC counters, indexed by NOC_ECC_MEM_PARITY / _HDR_SBE / _HDR_DBE. */
	uint32_t noc[NOC_ECC_NUM_SOURCES];
	/** NIU_CFG_0 tile-clock-off as read from hardware. */
	bool clock_gated;
	/** true if the tile was clocked, passed liveness, and @c l1_sbe / @c l1_dbe are valid. */
	bool l1_valid;
	uint32_t l1_sbe;
	uint32_t l1_dbe;
};

/**
 * @brief Read a Tensix tile's NIU ECC counters, clock gate, and L1 ECC counters together.
 *
 * One TLB program and one TLB lock hold per tile: the NIU block and ECC_CTRL/STATUS
 * share a NOC2AXI window. The gate bit is read first and the L1 view is skipped when it
 * is set, so this never reads behind a stopped tile clock. Caller holds
 * @ref NocEccStateLock so the gate cannot change between the two reads.
 *
 * @param noc_x NOC 0 X coordinate of the Tensix tile.
 * @param noc_y NOC 0 Y coordinate of the Tensix tile.
 * @param out   Filled in; @c l1_valid tells whether the L1 fields mean anything.
 */
void TensixEccReadTile(uint8_t noc_x, uint8_t noc_y, struct tensix_ecc_tile *out);

#endif /* TENSIX_ECC_H */
