/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_TENSTORRENT_BH_ARC_L2CPU_H_
#define LIB_TENSTORRENT_BH_ARC_L2CPU_H_

/* Also included by l2cpu_resident.S: keep the constants below assembler-friendly */
#ifndef __ASSEMBLER__
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#endif /* __ASSEMBLER__ */

#define L2CPU_NUM_CLUSTERS 4
#define L2CPU_NUM_HARTS    4

/*
 * Resident stub. At boot the firmware places a small program in the cluster's L3 "zero
 * device" window, releases the cluster, and leaves every hart parked in wfi polling the
 * mailbox below. Payloads (OpenSBI, device tree, kernel) are loaded into the cluster's
 * DRAM by the host (scripts/l2cpu_boot.py), which owns that layout; the firmware never
 * touches the DRAM itself.
 */
#define L2CPU_ZERO_DEVICE_BASE    0x0A000000ULL
#define L2CPU_RESIDENT_BASE       L2CPU_ZERO_DEVICE_BASE
#define L2CPU_MAILBOX_STUB_OFFSET 0x1000 /* mailbox relative to the stub's _start */
#define L2CPU_MAILBOX_BASE        (L2CPU_RESIDENT_BASE + L2CPU_MAILBOX_STUB_OFFSET)

#define L2CPU_MAILBOX_GO    0x00 /* u32: nonzero -> jump */
#define L2CPU_MAILBOX_ENTRY 0x08 /* u64: jump target */
#define L2CPU_MAILBOX_ARG   0x10 /* u64: passed in a1 */
#define L2CPU_MAILBOX_READY 0x18 /* u32[L2CPU_NUM_HARTS]: L2CPU_READY_MAGIC while parked */
#define L2CPU_MAILBOX_SIZE  0x28
#define L2CPU_READY_MAGIC   0x50415243 /* "PARC" */

/*
 * L2CPUCLK while releasing the harts from reset (must be below 300 MHz) and once running
 * (1750 MHz is the highest rate validated on silicon).
 */
#define L2CPU_RESET_RELEASE_CLK_MHZ 200
#define L2CPU_RUN_CLK_MHZ           1750

#ifndef __ASSEMBLER__

/*
 * Cluster control is static: one cluster is parked in the resident stub at init and the
 * host enters it once through the mailbox. Nothing else starts, stops or restarts a
 * cluster at runtime.
 */

/**
 * @brief The cluster parked in the resident stub at init, or -1 if there is none.
 *
 * While a cluster is resident, PLL4 must not be power-cycled: all four L2CPUCLKs share
 * its VCO, so gating or re-locking it stalls the harts mid-transaction, and the cluster
 * cannot be restarted afterwards. bh_set_l2cpu_enable() consults this.
 */
int l2cpu_resident_cluster(void);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

#endif /* LIB_TENSTORRENT_BH_ARC_L2CPU_H_ */
