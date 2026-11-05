#!/bin/sh
# Build the Amiga-side programs with the m68k cross compiler in WSL.
#   Like this, you lazy sod: wsl.exe -d Ubuntu-24.04 -- sh /mnt/c/LLM/Projects/AGA-PISTORM/tools/agaboot/build.sh
set -e
export PATH=/opt/amiga/bin:$PATH
cd "$(dirname "$0")"
m68k-amigaos-gcc -O2 -noixemul -o agaboot.new agaboot.c dopusmenu.c startmenu.c setup.c pointer.c picture.c rgbport.c sound.c monitor.c eq.c vm.c rec.c front.c wbmenu.c ahi16.c pad.c usbmouse.c usbfind.c
ls -l agaboot.new
# The WHDLoad stand-in for the installer's AmiKit choice (agaboot WHDWRAP) - a right bloody hack.
m68k-amigaos-gcc -O2 -noixemul -o whdwrap.new whdwrap.c
ls -l whdwrap.new
# The Sound monitor as a program of its own (SYS:Utilities/AGA-SoundMonitor).
m68k-amigaos-gcc -O2 -noixemul -o smon.new smon.c
ls -l smon.new
