#!/bin/bash

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Fetch everything needed to run Linux on an L2CPU cluster that SMC firmware
# has parked in its resident stub (see scripts/l2cpu_boot.py), from a
# tt-bh-linux release:
#
#   fw_jump.bin         OpenSBI
#   blackhole-card.dtb  device tree
#   Image               Linux kernel
#   rootfs.ext4         Debian riscv64 root filesystem
#   tt-bh-linux         host console/disk/network daemon, built from source
#
# The repository is cloned into a temporary directory just to build the
# daemon. The output directory defaults to build-tt-bh-linux/.
#
# Usage: fetch_tt_bh_linux.sh [-r RELEASE] [-o DIR]
set -euo pipefail

REPO=https://github.com/tenstorrent/tt-bh-linux
RELEASE=v0.11
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT_DIR=$SCRIPT_DIR/../build-tt-bh-linux

usage() {
  echo "Usage: $0 [-r RELEASE] [-o DIR]"
  echo "  -r RELEASE   tt-bh-linux release tag (default: $RELEASE)"
  echo "  -o DIR       output directory (default: <repo>/build-tt-bh-linux)"
  exit "${1:-0}"
}

while getopts "r:o:h" opt; do
  case $opt in
    r) RELEASE=$OPTARG ;;
    o) OUT_DIR=$OPTARG ;;
    h) usage ;;
    *) usage 1 ;;
  esac
done

for tool in git curl unzip make g++; do
  command -v "$tool" >/dev/null || { echo "ERROR: $tool is required"; exit 1; }
done

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
cd "$OUT_DIR"

# ---- release assets -------------------------------------------------------
fetch() {
  local name=$1
  if [ -f "$name" ]; then
    echo "$name already present, skipping"
    return
  fi
  echo "Downloading $name"
  curl -fL --progress-bar -o "$name.tmp" "$REPO/releases/download/$RELEASE/$name"
  mv "$name.tmp" "$name"
}

fetch fw_jump.bin
fetch blackhole-card.dtb
fetch Image

if [ -f rootfs.ext4 ]; then
  echo "rootfs.ext4 already present, skipping"
else
  fetch tt-bh-disk-image.zip
  unzip -o tt-bh-disk-image.zip
  rm -f tt-bh-disk-image.zip
  mv debian-riscv64.img rootfs.ext4
fi

# ---- host daemon ----------------------------------------------------------
if [ -x tt-bh-linux ]; then
  echo "tt-bh-linux daemon already built, skipping"
else
  for hdr in slirp/libslirp.h slirp/libvdeslirp.h; do
    if ! echo "#include <$hdr>" | g++ -x c++ -fsyntax-only - 2>/dev/null; then
      echo "ERROR: missing $hdr; install with: sudo apt install libslirp-dev libvdeslirp-dev"
      exit 1
    fi
  done

  SRC_DIR=$(mktemp -d)
  trap 'rm -rf "$SRC_DIR"' EXIT

  echo "Cloning tt-bh-linux $RELEASE to build the daemon"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$RELEASE" "$REPO" "$SRC_DIR"
  make -C "$SRC_DIR/console" tt-bh-linux
  cp "$SRC_DIR/console/tt-bh-linux" tt-bh-linux
fi

echo "Ready in $OUT_DIR."
