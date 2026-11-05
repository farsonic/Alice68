#!/bin/sh
# aga-hdmi.audio (AHI driver) with the m68k cross compiler in WSL.
#   wsl.exe -d Ubuntu-24.04 -- sh /mnt/c/LLM/Projects/AGA-PISTORM/tools/ahi/build.sh
# A library, not a program: no startup code, no C library; 68020 for CAS.
set -e
export PATH=/opt/amiga/bin:$PATH
cd "$(dirname "$0")"
m68k-amigaos-gcc -O2 -m68020 -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-unused-parameter \
    -nostartfiles -nostdlib -s -o aga-hdmi.audio aga-hdmi.c -lgcc
ls -l aga-hdmi.audio
# ahitest: the driver from a Shell, step by step (a program, with libnix)
m68k-amigaos-gcc -O2 -m68020 -Wall -Wno-pointer-sign -noixemul -s -o ahitest ahitest.c
ls -l ahitest
