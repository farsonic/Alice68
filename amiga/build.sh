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
build Alice68-USBLog usblog_start.S usblog.c lvo.S
build alice68hdf.device hdf/hdf_start.S hdf/alice68hdf.c hdf/hdf_core.c lvo.S
build Alice68-HDF hdf/hdfinfo_start.S hdf/hdfinfo.c lvo.S
build Alice68-NetCheck netcheck_start.S netcheck.c lvo.S
build alice68wifi.device wifi/wifi_start.S wifi/wifi.c wifi/sdio.c wifi/chip.c wifi/fmac.c wifi/lvox.S lvo.S
build Alice68-WiFi wifi/wifitool_start.S wifi/wifitool.c wifi/lvox.S lvo.S
build Alice68-Monitor monitor_start.S monitor.c wifi/lvox.S lvo.S
# alice68hdf.device runs from Emu68's ROM board, which is mapped read-only:
# it must not have writable static data
python3 -I - out/alice68hdf.c.o out/hdf_core.c.o out/hdf_start.S.o <<'PY'
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
