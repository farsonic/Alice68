#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The folders' FAT engine (amiga/hdf/fatfs.c) on the PC: FAT32 and FAT16 images
# from mkfs.fat, filled by mtools; after the test, mtools must read back what
# the mirror has, and fsck.fat must find nothing wrong.   tools/hdf-test/fatfs.sh
set -e
P=$(cd "$(dirname "$0")/../.." && pwd); T=${TMPDIR:-/tmp}/alice68-fatfs-test
rm -rf "$T"; mkdir -p "$T"
cc -O1 -g -Wall -Wextra -Werror -std=gnu11 -I"$P/amiga/hdf" -o "$T/fatfs_test" "$P/tools/hdf-test/fatfs_test.c" "$P/amiga/hdf/fatfs.c" "$P/amiga/hdf/hdf_core.c"
export MTOOLS_SKIP_CHECK=1 LC_ALL=C.UTF-8
python3 -I - "$T/src" <<'PY'
import os, random, sys
r = random.Random(68)
d = sys.argv[1] + "/Games"
os.makedirs(d + "/SUB"); os.makedirs(d + "/Many")
def put(p, n): open(p, "wb").write(bytes(r.getrandbits(8) for _ in range(n)))
put(d + "/Long File Name.txt", 12345)
put(d + "/readme.txt", 99)
put(d + "/SUB/BIG.BIN", 300000)
put(d + "/äpfel Ü.txt", 2048)
for i in range(300): put(d + "/Many/file number %03d.dat" % i, i * 7)
PY
fail=0
for fs in "32 1 40000" "16 4 20000"; do
    set -- $fs
    img=$T/fat$1.img; m=$T/mirror$1; out=$T/out$1
    mkfs.fat -F $1 -s $2 -C "$img" $3 >/dev/null
    mcopy -s -i "$img" "$T/src/Games" ::/
    cp -r "$T/src" "$m"
    printf 'FAT%-3s engine  ' $1
    if "$T/fatfs_test" "$img" "$m" > "$T/log$1"; then tail -1 "$T/log$1"; else grep -v '^  log' "$T/log$1" | tail -20; fail=1; fi
    printf 'FAT%-3s fsck    ' $1
    if fsck.fat -n "$img" > "$T/fsck$1" 2>&1 && ! grep -qiE 'differ|duplicate|lost|orphan|bad|invalid|free cluster summary|wrong|\?' "$T/fsck$1"; then echo "ALL OK"; else cat "$T/fsck$1"; fail=1; fi
    printf 'FAT%-3s mtools  ' $1
    mkdir -p "$out"; mcopy -s -n -i "$img" ::/Games "$out/"
    if diff -r "$m/Games" "$out/Games" > "$T/diff$1"; then echo "ALL OK"; else head -20 "$T/diff$1"; fail=1; fi
done
exit $fail
