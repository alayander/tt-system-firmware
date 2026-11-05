.. _ttfw_l2cpu_linux:

Linux on the L2CPU
==================

This page describes how the SMC firmware brings up a Blackhole L2CPU cluster
(a four-hart SiFive X280 complex) and how to run Linux on it from the host
using the payloads shipped by `tt-bh-linux <https://github.com/tenstorrent/tt-bh-linux>`_.

Overview
--------

Bring-up is split between the SMC and the host:

1. **SMC (at boot)**: ``l2cpu_boot()`` in ``lib/tenstorrent/bh_arc/l2cpu.c``
   picks the first usable cluster (not harvested, backing GDDR instance
   trained), enables the cluster's L3 cache, programs the reset vectors,
   releases the cluster from reset at 200 MHz and ramps it to 1750 MHz. The
   harts land in a small *resident stub* the SMC placed in a pinned way of
   the cluster's L3 and park there, each one marking itself ready in a
   mailbox and sleeping in ``wfi`` between polls. The cluster's DRAM is not
   involved while it is parked.
2. **Host (on demand)**: ``scripts/l2cpu_boot.py`` writes OpenSBI, a patched
   device tree and a kernel into the same DRAM over NOC0 and raises the
   mailbox ``go`` flag. The harts jump to OpenSBI and Linux boots.
3. **Host (while Linux runs)**: the ``tt-bh-linux`` console daemon serves the
   serial console, the virtio root disk and user-mode networking from the
   host.

The cluster's reset release is one-shot per chip reset (re-asserting it hangs
the NOC), so neither the SMC nor the host ever resets the cluster. A chip
reset (``tt-smi -r``) is the only way back to the parked state.

Memory layout
-------------

All addresses are offsets from ``L2CPU_DRAM_BASE`` (``0x4000_3000_0000``), the
cluster's view of its 4 GB GDDR-backed DRAM. The layout matches tt-bh-linux so
its daemon works unchanged.

.. list-table::
   :header-rows: 1
   :widths: 22 18 60

   * - Offset
     - Contents
     - Written by
   * - ``+0x000000``
     - OpenSBI ``fw_jump.bin``
     - host (``l2cpu_boot.py``)
   * - ``+0x100000``
     - device tree blob
     - host (``l2cpu_boot.py``)
   * - ``+0x200000``
     - Linux ``Image``
     - host (``l2cpu_boot.py``)
   * - top 6 MB
     - no-map reservation holding four ``virtio,mmio`` devices
     - described in the patched device tree, served by the daemon

The resident stub and its mailbox (``go``, ``entry``, ``arg``, ``ready[4]``)
are not in DRAM. They sit at ``0x0A00_0000`` and ``0x0A00_1000`` in the
cluster's *zero device* window: a cacheable address range that discards
writes and reads as zero, but which the L3 caches like ordinary memory. The
SMC steers its writes into one L3 way and then pins that way by clearing it
from every master's ``CCACHE0_WAYMASK0`` entry, so the lines stay resident
with no memory behind them. Once the harts have jumped, the way is unpinned
and returned to the cache.

All commands below are run from the root of the ``tt-system-firmware``
checkout.

Step 1: fetch the payloads and build the daemon
-----------------------------------------------

.. code-block:: bash

   scripts/fetch_tt_bh_linux.sh

This downloads ``fw_jump.bin``, ``blackhole-card.dtb``, ``Image`` and the
Debian riscv64 disk image from a tt-bh-linux release (``v0.11`` by default,
``-r TAG`` to pick another), renames the disk image to ``rootfs.ext4``, clones
the matching tag into a temporary directory to build the console daemon, and
leaves everything in ``build-tt-bh-linux/``:

.. code-block:: text

   build-tt-bh-linux/
     fw_jump.bin          OpenSBI
     blackhole-card.dtb   device tree
     Image                Linux kernel
     rootfs.ext4          root filesystem (virtio disk)
     tt-bh-linux          console / disk / network daemon

Step 2: confirm the cluster is parked
-------------------------------------

After a chip reset the SMC parks the cluster during its own boot. Check that
all four harts are waiting in the stub:

.. code-block:: bash

   tt-smi -r
   scripts/l2cpu_boot.py status
   # L2CPU0 (NOC0 8,3): harts parked in resident stub: 0xf

A mask other than ``0xf`` means the SMC did not release the cluster (check
the SMC log in ``dmesg`` for ``L2CPU``) or the harts already jumped; in both
cases a chip reset is required before continuing.

Step 3: load and start Linux
----------------------------

.. code-block:: bash

   scripts/l2cpu_boot.py boot

``boot`` is ``load`` followed by ``jump``. It:

- patches the device tree the same way tt-bh-linux's ``boot.py`` does
  (``/chosen/bootargs``, the no-map reservation and the four ``virtio,mmio``
  nodes);
- writes OpenSBI, the device tree and the kernel at the offsets above;
- writes ``entry``/``arg`` to the mailbox, reads them back, raises ``go`` and
  confirms every hart has left the stub.

Step 4: attach the console daemon
---------------------------------

Start the daemon **after** ``boot``. It looks for the debug descriptor OpenSBI
publishes at DRAM ``+0x80`` and exits with ``eye catcher mismatch`` if OpenSBI
has not run yet.

.. code-block:: bash

   build-tt-bh-linux/tt-bh-linux --ttdevice 0 --l2cpu 0 --disk build-tt-bh-linux/rootfs.ext4

The daemon prints the serial console (OpenSBI banner, kernel log, login
prompt), backs ``/dev/vda`` with ``rootfs.ext4`` and provides user-mode
networking with SSH forwarded to a host port (``2222`` for device 0,
cluster 0. see the tt-bh-linux README for the port formula and login
details). Stopping the daemon detaches the console and disk; Linux keeps
running and the daemon can be restarted.

Power management interactions
-----------------------------

``tt-kmd``'s power policy lowers the card to a low-power state whenever no
client holds the device open (by default 5 s after the last close), which
includes powering down the GDDR PHYs and gating the L2CPU clocks. The
firmware applies two rules:

- While ``l2cpu_released_mask()`` is non-zero, L2CPU clock-gating requests
  are refused (``bh_set_l2cpu_enable()`` returns ``-EBUSY`` and logs
  ``L2CPU power-off refused``). The parked harts live in the L2CPU tile.
- While a released cluster has left the stub (``l2cpu_running_gddr_mask()``
  reads its mailbox and finds fewer than four harts parked), the GDDR
  instance backing it is kept out of the PHY powerdown
  (``set_mrisc_power_setting()`` logs ``Keeping GDDR 0x.. awake``); the
  remaining instances follow the normal policy.

Consequence: a parked cluster costs only its L2CPU clock domain, and all
eight GDDR instances follow the normal power policy. From the moment the
harts jump until the next chip reset, the cluster's GDDR instance (GDDR5 for
cluster 0) stays powered, because Linux is executing from it. The other seven
instances, the Tensix clocks and AICLK are unaffected throughout.

Resetting and rebooting
-----------------------

There is no soft reboot of the cluster. To boot Linux again (new kernel,
clean root filesystem, after a crash):

.. code-block:: bash

   tt-smi -r                # chip reset: SMC re-parks the cluster
   python scripts/l2cpu_boot.py boot
   ./build-tt-bh-linux/tt-bh-linux --ttdevice 0 --l2cpu 0 --disk build-tt-bh-linux/rootfs.ext4
