#!/bin/bash
# The fake card service (fakecard.c) -> amiga/out/test/alice68hdf.device, for trying Alice68Prefs's
# and `Alice68 CARD`'s card pages on an ordinary Amiga or FS-UAE: copy it to DEVS:alice68hdf.device and
# give a folder laid out as Alice68's FAT partition the volume name (or assign) FAKECARD:.
# SPDX-License-Identifier: GPL-2.0-only
set -e
cd "$(dirname "$0")/../.."
CF="-m68020 -O2 -fomit-frame-pointer -fno-pic -fno-pie -ffreestanding -fno-builtin -nostdlib -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -Wextra -Werror -std=gnu11"
O=out/test; mkdir -p $O
for s in device_start.S test/fakecard/fakecard.c lvo.S wifi/lvox.S; do m68k-linux-gnu-gcc $CF -c $s -o $O/fc_$(basename $s).o; done
m68k-linux-gnu-ld -T link.ld --emit-relocs -o $O/fakecard.elf $O/fc_device_start.S.o $O/fc_fakecard.c.o $O/fc_lvo.S.o $O/fc_lvox.S.o 2>/dev/null
python3 -I elf2hunk.py $O/fakecard.elf $O/alice68hdf.device
