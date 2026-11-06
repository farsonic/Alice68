#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# alice68wifi.device's protocol layer on the PC (expect "ALL OK")
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/alice68-fmac-test
cc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I../../third_party/Emu68/src/alice/wifi -o "$out" fmac_test.c ../../third_party/Emu68/src/alice/wifi/fmac.c
"$out"
