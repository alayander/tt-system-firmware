/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host message handler for TT_SMC_MSG_EFUSE_BURN_BITS (0x1F).
 *
 * The bit-strobing lives in the tt_efuse driver; this handler owns
 * the eFuse program-voltage (VQPS) supply, applied over I2C power switches.
 *
 * eFuse programming is IRREVERSIBLE. This translation unit is compiled (and the
 * message registered) only when CONFIG_TT_BH_ARC_EFUSE_BURN is set, which is
 * scoped to specific board revisions via Kconfig.defconfig.
 */

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/bh_efuse.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

#include "dw_apb_i2c.h"
#include "timer.h"

LOG_MODULE_REGISTER(efuse_burn, CONFIG_TT_APP_LOG_LEVEL);

/* eFuse program-voltage (VQPS) power switches. */
#define EFUSE_POWER_SWITCH0_ADDR 0x72
#define EFUSE_POWER_SWITCH1_ADDR 0x73
#define EFUSE_CTRL_REG_ADDR      0x5
#define EFUSE_I2C_MST_ID         2
#define EFUSE_CMD_BYTE_SIZE      1
#define EFUSE_DATA_BYTE_SIZE     1
#define VQPS_HI                  1
#define VQPS_LO                  0

/* Burn data occupies request words data[2..7], so this many bits fit. */
#define EFUSE_BURN_MAX_BITS ((REQUEST_MSG_LEN - 2) * 32)

#if DT_NODE_EXISTS(DT_NODELABEL(efuse))
static const struct device *const efuse_dev = DEVICE_DT_GET(DT_NODELABEL(efuse));
#else
static const struct device *const efuse_dev;
#endif

static void ForceEfuseSupply(uint32_t level)
{
	uint8_t mode = 0xf << 4;
	uint8_t efuse_ctrl = (level == VQPS_HI) ? 0xf : 0x0;

	efuse_ctrl |= mode;

	I2CInit(I2CMst, EFUSE_POWER_SWITCH0_ADDR, I2CFastMode, EFUSE_I2C_MST_ID);
	I2CWriteBytes(EFUSE_I2C_MST_ID, EFUSE_CTRL_REG_ADDR, EFUSE_CMD_BYTE_SIZE, &efuse_ctrl,
		      EFUSE_DATA_BYTE_SIZE);

	I2CInit(I2CMst, EFUSE_POWER_SWITCH1_ADDR, I2CFastMode, EFUSE_I2C_MST_ID);
	I2CWriteBytes(EFUSE_I2C_MST_ID, EFUSE_CTRL_REG_ADDR, EFUSE_CMD_BYTE_SIZE, &efuse_ctrl,
		      EFUSE_DATA_BYTE_SIZE);
}

static uint8_t efuse_burn_bits_handler(const union request *request, struct response *response)
{
	ARG_UNUSED(response);

	uint32_t efuse_box_id = (request->data[0] >> 8) & 0xf;
	uint16_t start_bit_addr = request->data[1] & 0xffff;
	uint8_t bit_count = (request->data[1] >> 24) & 0xff;
	const uint32_t *burn_data = &request->data[2];

	/* Guard against reading past the request payload. */
	if (bit_count > EFUSE_BURN_MAX_BITS) {
		return 1;
	}

	ForceEfuseSupply(VQPS_HI);
	WaitMs(1);

	int rc = tt_bh_efuse_burn(efuse_dev, (enum tt_bh_efuse_box_id)efuse_box_id, start_bit_addr,
				  bit_count, burn_data);

	ForceEfuseSupply(VQPS_LO);

	if (rc != 0) {
		LOG_ERR("eFuse burn failed: box=%u start=%u count=%u rc=%d", efuse_box_id,
			start_bit_addr, bit_count, rc);
		return 1;
	}

	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_EFUSE_BURN_BITS, efuse_burn_bits_handler);
