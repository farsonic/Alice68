#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The kernel's USB mouse report parser on the PC (expect "ALL OK")
set -e
cd "$(dirname "$0")"
X=../../third_party/Emu68/src/frf_usb/xhci
out=${TMPDIR:-/tmp}/alice68-hid-test
cc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -I$X -o "$out" hid_test.c $X/alice_hid.c
"$out"
