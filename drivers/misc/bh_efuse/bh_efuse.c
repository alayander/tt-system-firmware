/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_bh_efuse

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/bh_efuse.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(tt_bh_efuse, CONFIG_TT_BH_EFUSE_LOG_LEVEL);

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 0

#define EFUSE_BOX_ADDR_ALIGN 0x2000u

struct tt_bh_efuse_config {
	uintptr_t mem_base_addr;
	uintptr_t cntl_reg_map_base_addr;
};

#define EFUSE_CTRL_REG_START_ADDR(cfg, box_id)                                                     \
	((cfg)->cntl_reg_map_base_addr + ((uint32_t)(box_id) * EFUSE_BOX_ADDR_ALIGN))

#define EFUSE_RD_CNTL_REG_OFFSET 0x0u
#define EFUSE_WR_CNTL_REG_OFFSET 0x4u
#define EFUSE_DATA_REG_OFFSET    0xCu

#define EFUSE_ROW_SIZE      32u
#define EFUSE_BOX_SIZE_BITS 8192u

#define GET_EFUSE_CNTL_ADDR(cfg, box_id, reg_name)                                                 \
	(EFUSE_##reg_name##_REG_OFFSET + EFUSE_CTRL_REG_START_ADDR(cfg, box_id))

typedef struct {
	uint32_t csb: 1;
	uint32_t load: 1;
	uint32_t rsvd_0: 6;
	uint32_t strobe: 1;
	uint32_t rsvd_1: 7;
	uint32_t addr: 13;
	uint32_t rsvd_2: 2;
	uint32_t ovrd: 1;
} EFUSE_CNTL_EFUSE_RD_CNTL_reg_t;

typedef union {
	uint32_t val;
	EFUSE_CNTL_EFUSE_RD_CNTL_reg_t f;
} EFUSE_CNTL_EFUSE_RD_CNTL_reg_u;

#define EFUSE_CNTL_EFUSE_RD_CNTL_REG_DEFAULT 0x00000001u

typedef struct {
	uint32_t csb: 1;
	uint32_t pgenb: 1;
	uint32_t ps: 1;
	uint32_t rsvd_0: 5;
	uint32_t strobe: 1;
	uint32_t rsvd_1: 7;
	uint32_t row_addr: 8;
	uint32_t col_addr: 5;
	uint32_t rsvd_2: 2;
	uint32_t ovrd: 1;
} EFUSE_CNTL_EFUSE_WR_CNTL_reg_t;

typedef union {
	uint32_t val;
	EFUSE_CNTL_EFUSE_WR_CNTL_reg_t f;
} EFUSE_CNTL_EFUSE_WR_CNTL_reg_u;

/* Reset default: csb=1, pgenb=1 (both active-low signals deasserted). */
#define EFUSE_CNTL_EFUSE_WR_CNTL_REG_DEFAULT 0x00000003u

static int tt_bh_efuse_read_impl(const struct device *dev, enum tt_bh_efuse_access_type acc_type,
				 enum tt_bh_efuse_box_id box_id, uint32_t offset, uint32_t *value)
{
	const struct tt_bh_efuse_config *cfg = dev->config;

	if ((uint32_t)box_id >= TT_BH_EFUSE_BOX_ID_NUM) {
		return -EINVAL;
	}

	if (acc_type == TT_BH_EFUSE_ACCESS_DIRECT) {
		uintptr_t efuse_addr = cfg->mem_base_addr +
				       ((uint32_t)box_id * EFUSE_BOX_ADDR_ALIGN) +
				       (offset * sizeof(uint32_t));

		*value = sys_read32(efuse_addr);
		return 0;
	}

	if (acc_type != TT_BH_EFUSE_ACCESS_INDIRECT) {
		return -EINVAL;
	}

	EFUSE_CNTL_EFUSE_RD_CNTL_reg_u efuse_rd_cntl_reg;

	efuse_rd_cntl_reg.val = EFUSE_CNTL_EFUSE_RD_CNTL_REG_DEFAULT;
	efuse_rd_cntl_reg.f.csb = 0;
	efuse_rd_cntl_reg.f.load = 1;
	efuse_rd_cntl_reg.f.addr = offset;
	efuse_rd_cntl_reg.f.ovrd = 1;

	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, RD_CNTL));
	k_busy_wait(1);

	efuse_rd_cntl_reg.f.strobe = 1;
	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, RD_CNTL));
	k_busy_wait(1);
	efuse_rd_cntl_reg.f.strobe = 0;
	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, RD_CNTL));
	k_busy_wait(1);

	efuse_rd_cntl_reg.f.ovrd = 0;
	sys_write32(efuse_rd_cntl_reg.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, RD_CNTL));

	*value = sys_read32(GET_EFUSE_CNTL_ADDR(cfg, box_id, DATA));
	return 0;
}

/* Blow a single eFuse bit at (row, col). Assumes VQPS is already applied. */
static void efuse_burn_bit(const struct tt_bh_efuse_config *cfg, enum tt_bh_efuse_box_id box_id,
			   uint32_t row_addr, uint32_t col_addr)
{
	EFUSE_CNTL_EFUSE_WR_CNTL_reg_u wr;

	wr.val = EFUSE_CNTL_EFUSE_WR_CNTL_REG_DEFAULT;

	/* 1. Take control of the bus and ramp program-select up (CSB stays high). */
	wr.f.ovrd = 1;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	wr.f.ps = 1;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	k_busy_wait(1);

	/* 2. Assert chip-select and program-enable, select the bit address. */
	wr.f.csb = 0;
	wr.f.pgenb = 0;
	wr.f.row_addr = row_addr;
	wr.f.col_addr = col_addr;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	k_busy_wait(1);

	/* 3. Strobe the burn pulse (~5 us). */
	wr.f.strobe = 1;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	k_busy_wait(5);
	wr.f.strobe = 0;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	k_busy_wait(1);

	/* 4. Deassert chip-select and program-enable. */
	wr.f.csb = 1;
	wr.f.pgenb = 1;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	k_busy_wait(1);

	/* 5. Lower program-select. */
	wr.f.ps = 0;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
	k_busy_wait(1);

	/* 6. Release the access bus. */
	wr.f.ovrd = 0;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));
}

static int tt_bh_efuse_burn_impl(const struct device *dev, enum tt_bh_efuse_box_id box_id,
				 uint16_t start_bit_addr, uint8_t bit_count,
				 const uint32_t *burn_data)
{
	const struct tt_bh_efuse_config *cfg = dev->config;

	if (burn_data == NULL) {
		return -EINVAL;
	}

	if ((uint32_t)box_id >= TT_BH_EFUSE_BOX_ID_NUM) {
		return -EINVAL;
	}

	if ((uint32_t)start_bit_addr + bit_count > EFUSE_BOX_SIZE_BITS) {
		return -EINVAL;
	}

	EFUSE_CNTL_EFUSE_WR_CNTL_reg_u wr;

	wr.val = EFUSE_CNTL_EFUSE_WR_CNTL_REG_DEFAULT;
	sys_write32(wr.val, GET_EFUSE_CNTL_ADDR(cfg, box_id, WR_CNTL));

	for (uint32_t bit = 0; bit < bit_count; bit++) {
		uint32_t linear = (uint32_t)start_bit_addr + bit;
		uint32_t row_addr = linear / EFUSE_ROW_SIZE;
		uint32_t col_addr = linear % EFUSE_ROW_SIZE;
		uint32_t data = (burn_data[bit / 32] >> (bit % 32)) & 1u;

		if (data == 1u) {
			efuse_burn_bit(cfg, box_id, row_addr, col_addr);
		}
	}

	return 0;
}

static int tt_bh_efuse_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static DEVICE_API(tt_efuse, tt_bh_efuse_api) = {
	.read = tt_bh_efuse_read_impl,
	.burn = tt_bh_efuse_burn_impl,
};

#define TT_BH_EFUSE_DEFINE(inst)                                                                   \
	static const struct tt_bh_efuse_config tt_bh_efuse_cfg_##inst = {                          \
		.mem_base_addr = DT_INST_REG_ADDR_BY_NAME(inst, mem),                              \
		.cntl_reg_map_base_addr = DT_INST_REG_ADDR_BY_NAME(inst, cntl),                    \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, tt_bh_efuse_init, NULL, NULL, &tt_bh_efuse_cfg_##inst,         \
			      POST_KERNEL, CONFIG_TT_BH_EFUSE_INIT_PRIORITY, &tt_bh_efuse_api);

DT_INST_FOREACH_STATUS_OKAY(TT_BH_EFUSE_DEFINE)

#endif /* DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 0 */
