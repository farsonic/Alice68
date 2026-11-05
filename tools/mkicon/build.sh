#!/bin/sh
# wsl.exe -d Ubuntu-24.04 -- sh /mnt/c/LLM/Projects/AGA-PISTORM/tools/mkicon/build.sh
set -e
export PATH=/opt/amiga/bin:$PATH
cd "$(dirname "$0")"
m68k-amigaos-gcc -O2 -noixemul -o mkicon mkicon.c
ls -l mkicon
