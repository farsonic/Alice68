#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Alice68 hardfile tests: test cards, the core on the PC, the driver on a 68k emulator.
#   tools/hdf-test/run.sh   (after amiga/build.sh; needs mkfs.fat, mtools, sfdisk, machine68k)
set -e
P=$(cd "$(dirname "$0")/../.." && pwd); T=${TMPDIR:-/tmp}/alice68-hdf-test
rm -rf "$T"; mkdir -p "$T"
python3 -I "$P/tools/hdf-test/mkimg.py" "$T"
cc -O1 -Wall -Wextra -Werror -std=gnu11 -I"$P/amiga/hdf" -o "$T/hdf_test" "$P/tools/hdf-test/hdf_test.c" "$P/amiga/hdf/hdf_core.c"
run() {   # card, harness options
    printf '%-12s driver %-14s ' "$1" "${2:-default}"
    python3 "$P/amiga/test/harness_hdf.py" "$P/amiga/out/alice68hdf.device" "$P/amiga/out/alice68hdf.device.elf" "$T/$1.img" "$T" $2 | tail -1
}
for c in card32 card16 card32_boot card32_raw card32_off card32_ro card32_noboot card32_bootnb card32_extra; do
    printf '%-12s core   %-14s ' $c ''; "$T/hdf_test" "$T/$c.img" "$T" | tail -1
done
for c in card32 card16; do run $c; run $c nofsres; run $c no64; run $c cardro; done
for c in card32_boot card32_raw card32_off card32_ro card32_noboot card32_bootnb card32_extra; do run $c; done
run card32_extra nofsres
run card32_boot nofsres
# the folders' FAT engine, checked with fsck.fat and mtools
sh "$P/tools/hdf-test/fatfs.sh"
