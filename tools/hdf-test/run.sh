#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Alice68 hardfile, folder and CD image tests: test cards, the cores on the PC, the driver on a 68k emulator.
#   tools/hdf-test/run.sh   (after amiga/build.sh; needs mkfs.fat, mtools, sfdisk, genisoimage, machine68k;
#   exfatprogs, exfat-fuse and root for the exFAT tests, which are skipped without them)
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
# Amiga attributes in FS-UAE-style .uaem sidecars and %xx names (uaem.c)
cc -O1 -Wall -Wextra -Werror -std=gnu11 -I"$P/amiga/hdf" -o "$T/uaem_test" "$P/tools/hdf-test/uaem_test.c" "$P/amiga/hdf/uaem.c"
printf '%-12s core   %-14s ' uaem ''; "$T/uaem_test" | tail -1
# the folders' FAT engine, checked with fsck.fat and mtools
sh "$P/tools/hdf-test/fatfs.sh"
# the folders as volumes: alice68hdf.device's DOS handler on the 68k emulator
for o in '' cardro; do
    printf '%-12s driver %-14s ' folders "${o:-default}"
    python3 "$P/amiga/test/harness_folders.py" "$P/amiga/out/alice68hdf.device" "$P/amiga/out/alice68hdf.device.elf" "$T" $o | tail -1
done
# CD images: the ISO 9660 reader against genisoimage's images, then the volumes on the 68k emulator
sh "$P/tools/hdf-test/iso.sh"
for o in '' isooff; do
    printf '%-12s driver %-14s ' isos "${o:-default}"
    python3 "$P/amiga/test/harness_isos.py" "$P/amiga/out/alice68hdf.device" "$P/amiga/out/alice68hdf.device.elf" "$T" $o | tail -1
done
# exFAT: the reader against mkfs.exfat / exfat-fuse cards (fsck.exfat checked), then CD images on an
# exFAT partition through the driver on the 68k emulator (both need root, losetup and exfat-fuse)
sh "$P/tools/hdf-test/exfat.sh"
if [ "$(id -u)" = 0 ] && command -v mount.exfat-fuse >/dev/null && command -v mkfs.exfat >/dev/null; then
    printf '%-12s driver %-14s ' isos exfat
    python3 "$P/amiga/test/harness_isos.py" "$P/amiga/out/alice68hdf.device" "$P/amiga/out/alice68hdf.device.elf" "$T" exfat | tail -1
    for o in exfat exfatrw exfatboot 'exfatboot hdfboot'; do
        printf '%-12s driver %-14s ' folders "$o"
        python3 "$P/amiga/test/harness_folders.py" "$P/amiga/out/alice68hdf.device" "$P/amiga/out/alice68hdf.device.elf" "$T" $o | tail -1
    done
else
    echo "isos/folders driver exfat          SKIPPED: needs root, losetup, mkfs.exfat and exfat-fuse"
fi
