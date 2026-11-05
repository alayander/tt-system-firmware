#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Load and start a payload on an L2CPU cluster parked by SMC firmware.

At boot the SMC releases the first usable L2CPU cluster into a small resident
stub held in a pinned way of the cluster's L3 cache, and leaves every hart
polling a mailbox next to it.

This script wakes the DRAM, writes a payload into it and sets the mailbox
so the harts jump to it, without touching the one-shot cluster reset.
No chip reset is needed between the SMC booting and Linux running.

The payload set is the one tt-bh-linux ships (OpenSBI fw_jump.bin,
blackhole-card.dtb, Linux Image), placed with the same DRAM layout and the
same device tree patching as its boot.py, so the tt-bh-linux console daemon
works unchanged. Reference scripts/fetch_tt_bh_linux.sh to build the payloads.
"""

import argparse
import struct
import sys
import time
from pathlib import Path

import pcie_utils

NUM_HARTS = 4

# DRAM layout, as expected by the OpenSBI fw_jump build tt-bh-linux ships
# (FW_JUMP_FDT_OFFSET=0x100000, FW_JUMP_OFFSET=0x200000). The firmware never
# touches the DRAM; this script owns the layout.
L2CPU_DRAM_BASE = 0x400030000000
# L2CPU_DRAM_BASE is the cached alias of the DRAM; clearing this bit gives the
# uncached alias, which bypasses the L3 and reflects the state of the DRAM itself
L2CPU_DRAM_CACHED_BIT = 0x400000000000
L2CPU_OPENSBI_OFFSET = 0x000000
L2CPU_DTB_OFFSET = 0x100000
L2CPU_KERNEL_OFFSET = 0x200000
# Resident stub and mailbox: in the L3 "zero device" window, not in DRAM.
# Keep in sync with lib/tenstorrent/bh_arc/l2cpu.h
L2CPU_RESIDENT_BASE = 0x0A000000
L2CPU_MAILBOX_BASE = L2CPU_RESIDENT_BASE + 0x1000
MAILBOX_GO = 0x00
MAILBOX_ENTRY = 0x08
MAILBOX_ARG = 0x10
MAILBOX_READY = 0x18
READY_MAGIC = 0x50415243  # "PARC"

# L3 cache controller: the SMC pins the way holding the stub by clearing it
# from every master's way mask; once the harts have jumped the way is handed
# back to the cache by restoring the masks
L3_WAYMASK_BASE = 0x02010800
L3_WAYMASK_STRIDE = 8
L3_NUM_MASTERS = 38
L3_WAYMASK_ALL = 0xFFFF

# A read of DRAM that is powered down comes back as 0xffffffff only after the
# NOC times out (~80 ms); a live read is well under a millisecond
DRAM_TIMEOUT_READ_S = 0.02
DRAM_WAKE_TIMEOUT = 10.0

# Where tt-bh-linux puts an initramfs (its --rootfs_dst), relative to the base
INITRAMFS_OFFSET = 0xB5000000

# Physical NOC0 coordinates of the L2CPU tiles. Host access must use these even
# when NOC translation is enabled: pyluwen's bulk noc_write path does not go
# through translation.
L2CPU_NOC0_X = 8
L2CPU_NOC0_Y = (3, 9, 5, 7)

# Host-side virtio devices served by the tt-bh-linux daemon: four 2 MB MMIO
# regions at the top of DRAM, inside a 6 MB no-map reservation
VIRTIO_COUNT = 4
VIRTIO_REGION_SIZE = 0x200000
VIRTIO_RESERVED_SIZE = 0x600000
VIRTIO_IRQ_BASE = 33

# Where scripts/fetch_tt_bh_linux.sh puts the release files
DEFAULT_PAYLOAD_DIR = Path(__file__).resolve().parent.parent / "build-tt-bh-linux"
DEFAULT_OPENSBI = DEFAULT_PAYLOAD_DIR / "fw_jump.bin"
DEFAULT_DTB = DEFAULT_PAYLOAD_DIR / "blackhole-card.dtb"
DEFAULT_KERNEL = DEFAULT_PAYLOAD_DIR / "Image"


class L2cpu:
    """One L2CPU cluster reached over NOC0 from the host."""

    def __init__(self, chip, index):
        self.chip = chip
        self.index = index
        self.x, self.y = L2CPU_NOC0_X, L2CPU_NOC0_Y[index]

    def __str__(self):
        return f"L2CPU{self.index} (NOC0 {self.x},{self.y})"

    def read32(self, addr):
        return self.chip.noc_read32(0, self.x, self.y, addr)

    def write32(self, addr, value):
        self.chip.noc_write32(0, self.x, self.y, addr, value)

    def write(self, addr, data):
        self.chip.noc_write(0, self.x, self.y, addr, data)

    def wait_dram(self, base, timeout=DRAM_WAKE_TIMEOUT):
        """
        Block until the cluster's DRAM answers reads promptly.

        While the cluster is parked tt-kmd may have its GDDR PHY powered down;
        requesting the MRISC domain makes the SMC wake it. Until that completes
        a read of the DRAM returns 0xffffffff after a long NOC timeout, and a
        bulk write backs up the PCIe write path and stalls the host, so no
        access is made before this returns. The probe goes through the uncached
        alias so an L3 hit cannot stand in for the DRAM.
        """
        probe = (base & ~L2CPU_DRAM_CACHED_BIT) + L2CPU_OPENSBI_OFFSET
        deadline = time.monotonic() + timeout
        waited = False
        while True:
            start = time.monotonic()
            value = self.read32(probe)
            if time.monotonic() - start < DRAM_TIMEOUT_READ_S and value != 0xFFFFFFFF:
                if waited:
                    print(f"{self}: DRAM awake")
                return
            if time.monotonic() > deadline:
                sys.exit(f"{self}: DRAM did not wake within {timeout} s")
            if not waited:
                print(f"{self}: waiting for DRAM to wake")
                waited = True
            time.sleep(0.05)

    def ready_mask(self):
        ready = L2CPU_MAILBOX_BASE + MAILBOX_READY
        mask = 0
        for hart in range(NUM_HARTS):
            if self.read32(ready + 4 * hart) == READY_MAGIC:
                mask |= 1 << hart
        return mask

    def unpin_resident_way(self):
        """Return the L3 way that held the stub to the cache."""
        for master in range(L3_NUM_MASTERS):
            reg = L3_WAYMASK_BASE + master * L3_WAYMASK_STRIDE
            self.write32(reg, L3_WAYMASK_ALL)
            self.write32(reg + 4, 0)
        self.read32(L3_WAYMASK_BASE)

    def jump(self, entry, arg, timeout):
        """Set the mailbox; return the mask of harts that failed to leave."""
        mailbox = L2CPU_MAILBOX_BASE

        # entry/arg first; the read back makes sure the posted writes have
        # landed before 'go' is raised, as the harts load entry as one 64-bit
        # word
        self.write(mailbox + MAILBOX_ENTRY, struct.pack("<QQ", entry, arg))
        self.read32(mailbox + MAILBOX_ARG + 4)
        self.write32(mailbox + MAILBOX_GO, 1)

        deadline = time.monotonic() + timeout
        while True:
            mask = self.ready_mask()
            if mask == 0:
                self.unpin_resident_way()
                return 0
            if time.monotonic() > deadline:
                return mask
            time.sleep(0.001)


def patch_dtb(dtb, bootargs, virtio):
    """
    Apply the same edits tt-bh-linux's boot.py makes before handing the
    device tree to the kernel: /chosen/bootargs, and unless virtio is False a
    no-map reservation at the top of DRAM holding four virtio,mmio devices
    """
    try:
        import libfdt
    except ImportError:
        sys.exit("pylibfdt is required: pip install pylibfdt")

    fdt = libfdt.Fdt(dtb)
    fdt.resize(len(dtb) + 2000)

    chosen = fdt.path_offset("/chosen", libfdt.QUIET_NOTFOUND)
    if chosen < 0:
        chosen = fdt.add_subnode(0, "chosen")
    fdt.setprop(chosen, "bootargs", bootargs.encode() + b"\0")

    if virtio:
        memory = fdt.path_offset("/memory@%x" % L2CPU_DRAM_BASE, libfdt.QUIET_NOTFOUND)
        if memory < 0:
            sys.exit("memory node not found in device tree")
        mem_start, mem_size = struct.unpack(">QQ", fdt.getprop(memory, "reg"))
        mem_end = mem_start + mem_size

        reserved = fdt.path_offset("/reserved-memory", libfdt.QUIET_NOTFOUND)
        if reserved < 0:
            reserved = fdt.add_subnode(0, "reserved-memory")
            fdt.setprop_u32(reserved, "#address-cells", 2)
            fdt.setprop_u32(reserved, "#size-cells", 2)
            fdt.setprop(reserved, "ranges", b"")
        region = fdt.add_subnode(
            reserved, "memory@%x" % (mem_end - VIRTIO_RESERVED_SIZE)
        )
        fdt.setprop(
            region,
            "reg",
            struct.pack(">QQ", mem_end - VIRTIO_RESERVED_SIZE, VIRTIO_RESERVED_SIZE),
        )
        fdt.setprop(region, "no-map", b"")

        soc = fdt.path_offset("/soc", libfdt.QUIET_NOTFOUND)
        plic = fdt.path_offset(
            "/soc/interrupt-controller@c000000", libfdt.QUIET_NOTFOUND
        )
        if soc < 0 or plic < 0:
            sys.exit("soc or plic node not found in device tree")
        plic_phandle = fdt.get_phandle(plic)
        if plic_phandle == 0:
            plic_phandle = libfdt.fdt_get_max_phandle(fdt._fdt) + 1
            fdt.setprop_u32(plic, "phandle", plic_phandle)

        for i in reversed(range(VIRTIO_COUNT)):
            addr = mem_end - VIRTIO_REGION_SIZE * (i + 1)
            node = fdt.add_subnode(soc, "virtio@%x" % addr)
            fdt.setprop_str(node, "compatible", "virtio,mmio")
            fdt.setprop(node, "reg", struct.pack(">QQ", addr, VIRTIO_REGION_SIZE))
            fdt.setprop_u32(node, "interrupts", VIRTIO_IRQ_BASE - i)
            fdt.setprop_u32(node, "interrupt-parent", plic_phandle)

    fdt.pack()
    return bytes(fdt.as_bytearray())


def read_file(path):
    path = Path(path)
    if not path.is_file():
        sys.exit(f"{path} not found")
    return path.read_bytes()


def build_payloads(args, base):
    """Return [(name, address, bytes)] for everything that goes into DRAM."""
    payloads = [
        ("OpenSBI", base + L2CPU_OPENSBI_OFFSET, read_file(args.opensbi)),
        ("kernel", base + L2CPU_KERNEL_OFFSET, read_file(args.kernel)),
    ]

    bootargs = "rw console=hvc0 earlycon=sbi"
    if args.initramfs:
        initramfs = read_file(args.initramfs)
        payloads.append(("initramfs", base + INITRAMFS_OFFSET, initramfs))
        bootargs += f" initrd=0x{base + INITRAMFS_OFFSET:x},{len(initramfs)}"
    else:
        bootargs += f" root=/dev/{args.boot_device}"
    if args.extra_bootargs:
        bootargs += " " + args.extra_bootargs

    dtb = patch_dtb(read_file(args.dtb), bootargs, virtio=not args.no_virtio)
    payloads.append(("DTB", base + L2CPU_DTB_OFFSET, dtb))
    print(f"bootargs: {bootargs}")
    return payloads


def cmd_status(l2cpu, args):
    mask = l2cpu.ready_mask()
    print(f"{l2cpu}: harts parked in resident stub: 0x{mask:x}")
    return 0


def cmd_load(l2cpu, args):
    payloads = build_payloads(args, args.base)
    l2cpu.wait_dram(args.base)
    for name, addr, data in payloads:
        print(f"writing {name}: {len(data)} bytes at 0x{addr:x}")
        l2cpu.write(addr, data)
    return 0


def cmd_jump(l2cpu, args):
    mask = l2cpu.ready_mask()
    if mask != (1 << NUM_HARTS) - 1:
        print(f"{l2cpu}: harts parked 0x{mask:x}; cluster is not fully parked")
        print("(never released by the SMC, or already jumped; reset the chip to retry)")
        return 1

    l2cpu.wait_dram(args.base)
    left = l2cpu.jump(args.entry, args.arg, args.timeout)
    if left:
        print(f"{l2cpu}: harts 0x{left:x} still parked after jump request")
        return 1
    print(f"{l2cpu}: jumped to 0x{args.entry:x} (a1 = 0x{args.arg:x})")
    return 0


def cmd_boot(l2cpu, args):
    mask = l2cpu.ready_mask()
    print(f"{l2cpu}: harts parked in resident stub: 0x{mask:x}")
    if mask != (1 << NUM_HARTS) - 1:
        print("cluster is not fully parked; reset the chip (tt-smi -r) and retry")
        return 1
    return cmd_load(l2cpu, args) or cmd_jump(l2cpu, args)


def hexint(s):
    return int(s, 0)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument(
        "-d", "--device", type=int, default=0, help="/dev/tenstorrent/N"
    )
    parser.add_argument("-l", "--l2cpu", type=int, default=0, choices=range(4))
    parser.add_argument(
        "--base",
        type=hexint,
        default=L2CPU_DRAM_BASE,
        help="L2CPU DRAM base the payloads are loaded at (default 0x%x)"
        % L2CPU_DRAM_BASE,
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="show which harts are parked in the stub")

    def add_jump_args(p):
        p.add_argument(
            "--entry",
            type=hexint,
            default=None,
            help="jump target (default: base, where OpenSBI is loaded)",
        )
        p.add_argument("--arg", type=hexint, default=0, help="value passed in a1")
        p.add_argument(
            "--timeout", type=float, default=0.1, help="seconds to wait for harts"
        )

    def add_load_args(p):
        p.add_argument(
            "--opensbi",
            default=DEFAULT_OPENSBI,
            help=f"OpenSBI fw_jump.bin (default: {DEFAULT_OPENSBI})",
        )
        p.add_argument(
            "--dtb",
            default=DEFAULT_DTB,
            help=f"device tree blob (default: {DEFAULT_DTB})",
        )
        p.add_argument(
            "--kernel",
            default=DEFAULT_KERNEL,
            help=f"kernel Image (default: {DEFAULT_KERNEL})",
        )
        p.add_argument(
            "--initramfs", help="boot from this initramfs instead of a virtio disk"
        )
        p.add_argument("--boot-device", default="vda", help="root device (default vda)")
        p.add_argument(
            "--extra-bootargs", default="", help="appended to the kernel command line"
        )
        p.add_argument(
            "--no-virtio",
            action="store_true",
            help="do not add the virtio,mmio devices",
        )

    add_load_args(
        sub.add_parser("load", help="write OpenSBI, DTB and kernel into DRAM")
    )
    add_jump_args(
        sub.add_parser("jump", help="make the parked harts jump to a payload")
    )
    boot = sub.add_parser("boot", help="load, then jump")
    add_load_args(boot)
    add_jump_args(boot)

    args = parser.parse_args()
    if getattr(args, "entry", None) is None:
        args.entry = args.base

    chip = pcie_utils.get_chip(args.device)
    # Wake the GDDR PHYs for the life of this process; the stub itself needs
    # only the L2CPU domain, which the SMC keeps up while the cluster is parked
    chip.set_power(aiclk=False, mrisc=True, tensix=False, l2cpu=True, pcie=True)
    l2cpu = L2cpu(chip, args.l2cpu)
    return {"status": cmd_status, "load": cmd_load, "jump": cmd_jump, "boot": cmd_boot}[
        args.cmd
    ](l2cpu, args)


if __name__ == "__main__":
    sys.exit(main())
