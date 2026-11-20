/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_BH_EFUSE_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_BH_EFUSE_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tt_bh_efuse_box_id {
	TT_BH_EFUSE_BOX_DFT0 = 0,
	TT_BH_EFUSE_BOX_DFT1 = 1,
	TT_BH_EFUSE_BOX_FUNC = 2,
	TT_BH_EFUSE_BOX_ID_NUM = 3,
};

enum tt_bh_efuse_access_type {
	TT_BH_EFUSE_ACCESS_INDIRECT = 0,
	TT_BH_EFUSE_ACCESS_DIRECT = 1,
};

__subsystem struct tt_efuse_driver_api {
	int (*read)(const struct device *dev, enum tt_bh_efuse_access_type acc_type,
		    enum tt_bh_efuse_box_id box_id, uint32_t offset, uint32_t *value);
	int (*burn)(const struct device *dev, enum tt_bh_efuse_box_id box_id,
		    uint16_t start_bit_addr, uint8_t bit_count, const uint32_t *burn_data);
};

/**
 * @brief Read a 32-bit eFuse word.
 *
 * Reads the word at @p offset (in 32-bit units) from the given eFuse box, either
 * directly from the box memory window or indirectly via the control registers.
 *
 * @param dev eFuse device.
 * @param acc_type Access method (@ref TT_BH_EFUSE_ACCESS_DIRECT or
 *                 @ref TT_BH_EFUSE_ACCESS_INDIRECT).
 * @param box_id Source eFuse box.
 * @param offset Word offset within the box (32-bit units).
 * @param[out] value Destination for the read word.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p value is NULL (or the driver rejects the box/access type).
 * @retval -ENODEV if the device is not ready.
 * @retval -ENOSYS if the driver does not implement reading.
 */
static inline int tt_bh_efuse_read(const struct device *dev, enum tt_bh_efuse_access_type acc_type,
				   enum tt_bh_efuse_box_id box_id, uint32_t offset, uint32_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}

	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}

	const struct tt_efuse_driver_api *api = DEVICE_API_GET(tt_efuse, dev);

	if (api == NULL || api->read == NULL) {
		return -ENOSYS;
	}

	return api->read(dev, acc_type, box_id, offset, value);
}

/**
 * @brief Burn (program) a run of eFuse bits.
 *
 * Bits are addressed linearly from @p start_bit_addr; for each of the next
 * @p bit_count bits whose corresponding bit in @p burn_data is 1, the fuse is
 * blown. This is irreversible. The caller is responsible for applying the eFuse
 * program voltage (VQPS) before and removing it after this call.
 *
 * @param dev eFuse device.
 * @param box_id Target eFuse box.
 * @param start_bit_addr Linear bit address of the first bit.
 * @param bit_count Number of consecutive bits to consider.
 * @param burn_data Bitmask (LSB-first) of which bits in the run to blow.
 *
 * @retval 0 on success.
 * @retval -EINVAL on a bad argument (null data, box id, or out-of-range range).
 * @retval -ENODEV if the device is not ready.
 * @retval -ENOSYS if the driver does not implement burning.
 */
static inline int tt_bh_efuse_burn(const struct device *dev, enum tt_bh_efuse_box_id box_id,
				   uint16_t start_bit_addr, uint8_t bit_count,
				   const uint32_t *burn_data)
{
	if (burn_data == NULL) {
		return -EINVAL;
	}

	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}

	const struct tt_efuse_driver_api *api = DEVICE_API_GET(tt_efuse, dev);

	if (api == NULL || api->burn == NULL) {
		return -ENOSYS;
	}

	return api->burn(dev, box_id, start_bit_addr, bit_count, burn_data);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_BH_EFUSE_H_ */
