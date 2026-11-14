#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The kernel's Bluetooth host on the PC against a scripted controller (expect "ALL OK")
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/alice68-bt-test
cc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I../../third_party/Emu68/src/alice/bt -o "$out" bt_test.c ../../third_party/Emu68/src/alice/bt/bthost.c
"$out" "$@"
