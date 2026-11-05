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
