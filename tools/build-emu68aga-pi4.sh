#!/bin/bash
# build-emu68aga-pi4.sh - EMU68AGA Bare Metal for the Raspberry Pi 4 / CM4 / Pi 400
# (AGA-PiStorm 1.120 virtual chipset + FRF standalone layer, no PiStorm, no Amiga).
# SPDX-License-Identifier: GPL-2.0-only
#
#   ARM_GNU_TOOLCHAIN=<arm-gnu-toolchain-15.x-...-aarch64_be-none-linux-gnu> \
#     bash tools/build-emu68aga-pi4.sh            -> dist/pi4/kernel8.img
#
# Needs: cmake ninja dtc xxd, powerpc-linux-gnu-gcc-14 (ppc ROM), and the Arm GNU
# Toolchain 15.x for big-endian AArch64 (GCC 14 miscompiles the vector code).
set -e
P=${AGA_PISTORM_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
SRC=$P/third_party/Emu68
OUT=${AGA_OUT:-$P/dist/pi4}
B=${AGA_BUILD:-/tmp/emu68aga-pi4-build}
USB=${FRF_USB:-ON}
[ -x "$ARM_GNU_TOOLCHAIN/bin/aarch64_be-none-linux-gnu-gcc" ] || { echo "set ARM_GNU_TOOLCHAIN"; exit 1; }
mkdir -p "$OUT"; rm -rf "$B"
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DTARGET=raspi64 -DVARIANT=pistorm-classic \
      -DCMAKE_TOOLCHAIN_FILE="$SRC/toolchains/aarch64_be-arm-gnu.cmake" \
      -DEMU68_SKIP_FIRMWARE=ON -DAGA_PISTORM="$P" -DAGA_PI3=OFF \
      -DFRF_VIRTUAL_AGA=ON -DFRF_USB_HOST=$USB -DFRF_USB_NATIVE_DWC2=$USB -DFRF_USB_TINYUSB=$USB \
      -DFRF_TINYUSB_MSC_WRITE=ON -DFRF_USB_DIAG_HOLD=OFF -DFRF_USB_PASSIVE_TEST=OFF \
      -DFRF_NET_Z2_BRIDGE=OFF -DFRF_RTG_HANDOFF=OFF -DFRF_WIFI_SD1_ENUM=OFF \
      -DAGA_DIAG=OFF -DAGA_PROBES=OFF > "$OUT/cfg.log" 2>&1 || { tail -30 "$OUT/cfg.log"; exit 1; }
ninja -C "$B" > "$OUT/build.log" 2>&1 || { grep -E "error|undefined|FAILED" "$OUT/build.log" | head -40; exit 1; }
zcat "$B/Emu68.img.gz" > "$OUT/kernel8.img"
W=$(head -c 4 "$OUT/kernel8.img" | xxd -p)
[ "${W:6:2}" = "14" ] || { echo "BAD first word $W"; exit 1; }
echo "kernel8.img: $(wc -c < "$OUT/kernel8.img") bytes, first word $W"
strings -a "$OUT/kernel8.img" | grep -m1 'VER: Emu68'
sha256sum "$OUT/kernel8.img"
