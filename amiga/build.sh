#!/bin/bash
# Build the AmigaOS side of Alice68's Pi 4 USB pass-through with a plain
# m68k ELF gcc (Ubuntu: apt install gcc-m68k-linux-gnu) - no Amiga NDK.
set -e
cd "$(dirname "$0")"
CC=${M68K_CC:-m68k-linux-gnu-gcc}
LD=${M68K_LD:-m68k-linux-gnu-ld}
CFLAGS="-m68020 -O2 -fomit-frame-pointer -fno-pic -fno-pie -ffreestanding -fno-builtin -nostdlib -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -Wextra -Werror -std=gnu11"
mkdir -p out
build() {   # name, sources...
    local name=$1; shift
    local objs=()
    for s in "$@"; do
        o=out/$(basename "$s").o
        $CC $CFLAGS -c "$s" -o "$o"
        objs+=("$o")
    done
    $LD -T link.ld --emit-relocs -o "out/$name.elf" "${objs[@]}"
    python3 -I elf2hunk.py "out/$name.elf" "out/$name"
}
build alice68-usb.device device_start.S alice68-usb.c lvo.S
python3 -I folder/mkdiskicons.py out            # the volumes' Disk.info, for alice68hdf.device
build alice68hdf.device hdf/hdf_start.S hdf/alice68hdf.c hdf/folders.c hdf/isos.c hdf/hdf_core.c hdf/fatfs.c hdf/iso9660.c hdf/exfat.c hdf/uaem.c hdf/cmdedit.c hdf/cardsvc.c out/disk_icons.c lvo.S
build alice68net.device device_start.S net/alice68net.c wifi/lvox.S lvo.S
# C:Alice68: the Shell command, one program with subcommands (alice68.c). Each subcommand is the
# tool it was (Alice68-Status, Alice68-USBLog, ...): compiled alone, then all its symbols made
# local but its main, so their own memcpy, dos, out ... do not meet.
sub() {   # source, main
    local o=out/sub_$(basename "$1").o
    $CC $CFLAGS -c "$1" -o "$o"
    m68k-linux-gnu-objcopy --keep-global-symbol="$2" "$o"
    subs+=("$o")
}
subs=()
for s in alice68_start.S alice68.c lvo.S wifi/lvox.S; do $CC $CFLAGS -c "$s" -o "out/a68_$(basename "$s").o"; subs+=("out/a68_$(basename "$s").o"); done
sub status.c status_main
sub usblog.c usblog_main
sub joytest.c joytest_main
sub wifi/wifitool.c wifitool_main
sub hdf/hdfinfo.c hdfinfo_main
sub netcheck.c netcheck_main
sub showconfig.c showconfig_main
sub monitor.c monitor_main
sub wheel.c wheel_main
$LD -T link.ld --emit-relocs -o out/Alice68.elf "${subs[@]}"
python3 -I elf2hunk.py out/Alice68.elf out/Alice68
build Alice68-NetworkGT netprefs_start.S netprefs.c lvoy.S wifi/lvox.S lvo.S
build Alice68-Card card_start.S card.c hdf/cmdedit.c ../third_party/Emu68/src/raspi/alice_opts.c lvoy.S wifi/lvox.S lvo.S
# Alice68-Network: the MUI window, built as Riptide's RiptidePrefs is (amiga/netgui/Makefile)
make -s -C netgui OUT="$(pwd)/out"
# the ALICE68 folder for the SD card: the files above, a ReadMe, Install and Uninstall
python3 -I folder/mkfolder.py out
# alice68hdf.device runs from Emu68's ROM board, which is mapped read-only:
# it must not have writable static data
python3 -I - out/alice68hdf.c.o out/folders.c.o out/isos.c.o out/hdf_core.c.o out/fatfs.c.o out/iso9660.c.o out/exfat.c.o out/uaem.c.o out/cmdedit.c.o out/cardsvc.c.o out/disk_icons.c.o out/hdf_start.S.o <<'PY'
import subprocess, sys
bad = 0
for o in sys.argv[1:]:
    for l in subprocess.run(["m68k-linux-gnu-objdump", "-h", o], capture_output=True, text=True).stdout.splitlines():
        f = l.split()
        if len(f) > 2 and (f[1].startswith(".data") or f[1].startswith(".bss")) and int(f[2], 16):
            print("%s: %s has %d bytes: writable static data is not allowed in a ROM module" % (o, f[1], int(f[2], 16)))
            bad = 1
sys.exit(bad)
PY

# the kernel embeds alice68hdf.device in Emu68's ROM board: regenerate its header
python3 -I - out/alice68hdf.device ../third_party/Emu68/src/boards/alice68hdf.device.h <<'PY'
import sys
b = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w", newline="\n") as f:
    f.write("/* SPDX-License-Identifier: GPL-2.0-only\n   alice68hdf.device (amiga/hdf), made by amiga/build.sh - do not edit. */\n")
    f.write("__attribute__((aligned(16))) unsigned char alice68hdf_device[] = {\n")
    for i in range(0, len(b), 12):
        f.write("  " + ", ".join("0x%02x" % x for x in b[i:i + 12]) + ",\n")
    f.write("};\nunsigned int alice68hdf_device_len = %d;\n" % len(b))
print("%s: %d bytes" % (sys.argv[2].split("/")[-1], len(b)))
PY
