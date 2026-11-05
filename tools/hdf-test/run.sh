#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Alice68 hardfile tests: test cards, the core on the PC, the driver on a 68k emulator.
#   tools/hdf-test/run.sh   (after amiga/build.sh; needs mkfs.fat, mtools, sfdisk, machine68k)
set -e
P=$(cd "$(dirname "$0")/../.." && pwd); T=${TMPDIR:-/tmp}/alice68-hdf-test
rm -rf "$T"; mkdir -p "$T"
python3 -I "$P/tools/hdf-test/mkimg.py" "$T"
cc -O1 -Wall -Wextra -Werror -std=gnu11 -I"$P/amiga/hdf" -o "$T/hdf_test" "$P/tools/hdf-test/hdf_test.c" "$P/amiga/hdf/hdf_core.c"
for c in card32 card16; do
    printf '%s core: ' $c; "$T/hdf_test" "$T/$c.img" "$T" | tail -1
    for o in default nofsres no64; do
        printf '%s driver %-8s ' $c $o
        python3 "$P/amiga/test/harness_hdf.py" "$P/amiga/out/alice68hdf.device" "$P/amiga/out/alice68hdf.device.elf" "$T/$c.img" "$T" $o | tail -1
    done
done
