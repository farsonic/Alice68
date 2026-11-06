#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Render the Alice68 splash on a PC: tools/splash-preview/build.sh [W H [MS]] > out.ppm
# (MS: the header's animation as it is MS milliseconds after it began)
set -e
D=$(cd "$(dirname "$0")" && pwd); E="$D/../../third_party/Emu68"
OUT=${TMPDIR:-/tmp}/alice-splash-preview
cc -O1 -Wall -Wextra -I"$E/src/raspi" -I"$E/include" -o "$OUT" "$D/preview.c" "$E/src/raspi/alice_splash.c" "$E/src/raspi/topaz.c"
"$OUT" "$@"
