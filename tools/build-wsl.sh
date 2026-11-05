#!/bin/bash
# build-wsl.sh - build the AGA-PISTORM kernels the way upstream builds Emu68.
# SPDX-License-Identifier: GPL-2.0-only
#
#   wsl.exe -d Ubuntu-24.04 -- bash /mnt/c/LLM/Projects/AGA-PISTORM/tools/build-wsl.sh
#
# WHY THIS EXISTS
# ---------------
# Emu68 must be built with the toolchain upstream's CI uses. Building it with
# the Arm "aarch64-none-elf" (newlib) toolchain produces a kernel that compiles,
# links, passes every static check - and does not boot. That cost an entire
# evening of hardware debugging: the kernel hung on the Emu68 logo, and because
# the AGA code was the obvious suspect, two real but unrelated bugs were found
# and fixed before anyone questioned the compiler.
#
# The tell was there all along. newlib rammed five source patches down my
# throat that upstream needs none of:
#
#   cmake/devicetree.cmake   make dtc optional            (dtc is packaged here)
#   external/CMakeLists.txt  __int64_t_defined for PRIx64 (glibc needs nothing)
#   src/math/log2.c          #undef log2, a newlib macro
#   src/support.c            printf -> kprintf shim
#   CMakeLists.txt           EMU68_SKIP_PPC_ROM, no ppc compiler
#
# Five workarounds to make a build succeed is not friction, it is evidence of
# being in the wrong environment. All five are reverted; none is needed here.
#
# Proof: the same commit built here boots; built with none-elf it does not.
# The uncompressed image is also exactly the size of the official release
# (4,239,392 bytes for plain beta.1), where the none-elf build was 4,251,680.
#
# ENVIRONMENT (matches .github/workflows/cmake.yml)
#   apt install build-essential ninja-build device-tree-compiler xxd cmake
#   apt install gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu
#   apt install gcc-14-powerpc-linux-gnu g++-14-powerpc-linux-gnu
#   cp /usr/aarch64-linux-gnu/include/gnu/stubs-lp64.h \
#      /usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h
#
# THE COMPILER FOR ALL THE KERNELS
# Upstream's gcc-14 (14.2) miscompiles big-endian vector code: the Pi 3's
# Sharp picture got dark lines, and a stereo left/right loop came out wrong at
# every tuning (see the AGA block in Emu68's CMakeLists.txt). The kernels
# are therefore built with the Arm GNU Toolchain 15.3.rel2 for
# big-endian AArch64, unpacked in ~/toolchains/armtc-15.3.rel2 (or
# ARM_GNU_TOOLCHAIN=<its folder>):
#   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
#   arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu.tar.xz
# The PiStorm16 kernel too, since 1.116's second tester build: on gcc-14 it
# was the one kernel still exposed to that bug (the stereo loop breaks at
# every tuning), and a tester's PiStorm16 on a CM4 hung and dropped HDMI
# sound with 1.116's first build. The ppc ROM needs powerpc-linux-gnu-gcc-14
# either way.
#
# NOTE ON THE FIRST WORD OF THE IMAGE
# A kernel starts with an AArch64 unconditional branch, so byte 3 is 0x14. The
# branch OFFSET varies between builds - the official release starts 28000014,
# my WSL beta.1 starts 20000014, and both boot. Do not check for an exact
# first word; check that byte 3 is 0x14.
#
# PISTORM16 / PISTORM32-LITE
#   wsl.exe -d Ubuntu-24.04 -- bash /mnt/c/LLM/Projects/AGA-PISTORM/tools/build-wsl.sh ps16
# builds Emu68's "pistorm" variant instead of "pistorm-classic" - one kernel for
# both newer boards, the model is detected at runtime. A classic kernel cannot
# drive them at all: different CPLD/FPGA protocol, it hangs on the logo. These
# go to dist/ps16, which the PiStorm16 package takes its kernel from; dist/wsl
# holds what the classic package ships and is never written in this mode.
#
# TEST KERNELS NEXT TO ANOTHER SESSION'S
#   AGA_OUT=<folder> puts the kernels and logs there instead of dist/wsl or
#   dist/ps16 (which the package takes its kernels from), and builds in
#   /tmp/aga-build-<folder's name>-* so two builds at once keep apart.
#   AGA_ONLY=1 builds the AGA kernels alone, without the stock and diag ones.
set -e

P=${AGA_PISTORM_ROOT:-/mnt/c/LLM/Projects/AGA-PISTORM}
SRC=$P/third_party/Emu68
OUT=${AGA_OUT:-$P/dist/wsl}
VARIANT=pistorm-classic
TOOLCHAIN=$SRC/toolchains/aarch64_be-arm-gnu.cmake
ARM_GNU=${ARM_GNU_TOOLCHAIN:-$HOME/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu}
if [ "$1" = "ps16" ]; then
    OUT=${AGA_OUT:-$P/dist/ps16}
    VARIANT=pistorm
fi
BTAG=${AGA_OUT:+$(basename "$AGA_OUT")-}
[ -x "$ARM_GNU/bin/aarch64_be-none-linux-gnu-gcc" ] \
    || { echo "missing: the Arm GNU Toolchain 15.3 in $ARM_GNU (see THE COMPILER above)"; exit 1; }
echo "compiler: $("$ARM_GNU/bin/aarch64_be-none-linux-gnu-gcc" --version | head -1)"
mkdir -p "$OUT"

# Published kernels leave the per-frame measurements out. For a test kernel:
#   wsl.exe -d Ubuntu-24.04 -- bash -c "AGA_PROBES=1 bash .../tools/build-wsl.sh"
PROBES=""
[ -n "$AGA_PROBES" ] && PROBES="-DAGA_PROBES=ON" && echo "AGA_PROBES: test kernels, with the per-frame measurements"

for t in aarch64-linux-gnu-gcc-14 powerpc-linux-gnu-gcc-14 dtc ninja cmake; do
    command -v "$t" >/dev/null 2>&1 || { echo "missing: $t (see ENVIRONMENT above)"; exit 1; }
done

build_one () {   # $1 = tag, $2 = extra cmake args, $3 = output filename
    local B=/tmp/aga-build-$BTAG$1
    rm -rf "$B"
    echo "=== $1: configure ==="
    cmake -S "$SRC" -B "$B" -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DTARGET=raspi64 -DVARIANT=$VARIANT \
          -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
          -DEMU68_SKIP_FIRMWARE=ON $2 \
          > "$OUT/cfg-$1.log" 2>&1 || { echo "CONFIGURE FAILED"; tail -25 "$OUT/cfg-$1.log"; return 1; }
    echo "=== $1: build ==="
    ninja -C "$B" > "$OUT/build-$1.log" 2>&1 \
        || { echo "BUILD FAILED"; grep -E "error|FAILED" "$OUT/build-$1.log" | head -20; return 1; }

    local IMG=$B/Emu68.img.gz
    [ -f "$IMG" ] || { echo "no Emu68.img.gz produced"; return 1; }
    local WORD=$(zcat "$IMG" | head -c 4 | xxd -p)
    [ "${WORD:6:2}" = "14" ] || { echo "  BAD: first word $WORD is not a branch"; return 1; }

    cp -f "$IMG" "$OUT/$3"
    echo "  $3"
    echo "    first word : $WORD"
    echo "    \$VER       : $(zcat "$IMG" | strings -a | grep -m1 'VER:')"
    echo "    size       : $(zcat "$IMG" | wc -c) bytes uncompressed"
}

if [ "$VARIANT" = "pistorm" ]; then
    # The control again: plain 1.1 for the same boards. If this one hangs too,
    # the card's config.txt is the problem, not the AGA code.
    [ -n "$AGA_ONLY" ] || build_one ps16-stock ""                  Emu68-pistorm16-1.1-stock.gz
    build_one ps16-aga   "-DAGA_PISTORM=$P $PROBES"  Emu68-pistorm16-aga.gz
    echo "DONE -> $OUT"
    exit 0
fi

# Plain beta.1, no AGA code. Keep building this: it is the control that says
# whether a boot failure is my fucking fault or the environment's.
[ -n "$AGA_ONLY" ] || build_one stock ""                                              Emu68-pistorm-classic-wsl-beta1.gz

build_one aga   "-DAGA_PISTORM=$P $PROBES"                              Emu68-pistorm-classic-aga-wsl.gz

# The Pi 3 kernel: the same code with the Pi 3 settings compiled in (VideoCore IV
# display lists, MMU chip RAM map, 37 ms audio lead, frame skip). The Pi 4 one
# above leaves them out; each still runs on the other Pi and says so.
build_one aga-pi3 "-DAGA_PISTORM=$P -DAGA_PI3=ON $PROBES"                Emu68-pistorm-classic-aga-pi3-wsl.gz

# AGA_DIAG is a cmake option (declared in CMakeLists.txt next to AGA_PISTORM).
# It used to be passed as -DCMAKE_C_FLAGS=-DAGA_DIAG on the belief that an
# option "silently does nothing" - the truth is the other way round. Emu68 sets
# CMAKE_C_FLAGS itself, so a value handed in on the command line was discarded,
# and the diag kernel came out uninstrumented: same uncompressed size as the
# shipping one, and traps_read/traps_write reading 0 in every measurement made
# with it. Verify after changing this: `grep AGA_DIAG /tmp/aga-build-diag/build.ninja`
# must match, and the two kernels must differ in size.
[ -n "$AGA_ONLY" ] || build_one diag  "-DAGA_PISTORM=$P -DAGA_DIAG=ON"   Emu68-pistorm-classic-diag-wsl.gz

# Bisect build: core 3 never runs the chipset loop; the trap path is untouched.
# Answers "core 0 or core 3" without a serial console.

# Bisect: skip the mmu_map at 68k address 0. That map is present in every
# configuration that fails and absent from the only one that works.

# Bisect: skip the 2MB chip RAM copy, the last substantial step in sandbox entry.

# Bisect: traps pass through to the real chipset. The last element common to
# every failure, and the one machine mode also enables from boot.

echo "DONE -> $OUT"
