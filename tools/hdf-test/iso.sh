#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Alice68 CD images: amiga/hdf/iso9660.c on the PC, against images made by genisoimage
# (Rock Ridge with deep and long names, Joliet, plain ISO 9660), compared with the source trees.
#   tools/hdf-test/iso.sh   (needs genisoimage)
set -e
P=$(cd "$(dirname "$0")/../.." && pwd); T=${TMPDIR:-/tmp}/alice68-iso-test
rm -rf "$T"; mkdir -p "$T"
cc -O1 -Wall -Wextra -Werror -std=gnu11 -I"$P/amiga/hdf" -o "$T/iso_test" "$P/tools/hdf-test/iso_test.c" "$P/amiga/hdf/iso9660.c" "$P/amiga/hdf/hdf_core.c"
python3 -I - "$T" <<'PY'
import os, random, subprocess, sys, zlib
T = sys.argv[1]
rnd = random.Random(1987)
def blob(n): return bytes(rnd.getrandbits(8) for _ in range(n))
long_name = 'A very long Rock Ridge file name ' + 'x' * 160 + '.txt'          # 197 characters: a CE area
trees = {
    'rr': {
        'Read Me.txt': b'Alice68 CD image test\n',
        'Mixed Case Name.DAT': blob(300000),
        'empty.bin': b'',
        'a/b/c/d/e/f/g/h/i/j/deep file.txt': b'ten folders down\n',
        long_name: b'long\n',
        'Ünïcode ä.txt': b'latin-1 name\n',
        'Docs/sub dir/file.txt': blob(5000),
        'Folder.With.Dots/x': b'x',
    },
    'joliet': {
        'Read Me.txt': b'Alice68 CD image test\n',
        'Mixed Case Name.DAT': blob(70000),
        'empty.bin': b'',
        'Docs/sub dir/file.txt': blob(2049),
        'A Joliet name of seventy characters, which ISO 9660 cannot hold xx.txt': b'j\n',
        'Ünïcode ä.txt': b'ucs-2 name\n',
    },
    'plain': {
        'README.TXT': b'plain\n',
        'DATA/FILE1.BIN': blob(4096 * 3 + 5),
        'DATA/SUB/F2.TXT': b'two\n',
        'NOEXT': b'no extension\n',
    },
}
opts = {'rr': ['-R', '-J', '-joliet-long'], 'joliet': ['-J', '-joliet-long'], 'plain': []}
def amiga(name):                                     # what the Amiga side sees
    n = ''.join(c if ord(c) < 0x100 else '?' for c in name).replace(':', '_')
    return n[:107]
fails = 0
for kind, files in trees.items():
    src = os.path.join(T, 'src-' + kind)
    for p, b in files.items():
        os.makedirs(os.path.dirname(os.path.join(src, p)) or src, exist_ok=True)
        open(os.path.join(src, p), 'wb').write(b)
    img = os.path.join(T, kind + '.iso')
    subprocess.run(['genisoimage', '-quiet', '-input-charset', 'utf-8', '-V', 'Alice68 Test', '-o', img] + opts[kind] + [src],
                   check=True, stderr=subprocess.DEVNULL)
    out = subprocess.run([os.path.join(T, 'iso_test'), img], capture_output=True).stdout.decode('latin-1').splitlines()
    want = set()
    for p, b in files.items():
        parts = [amiga(c) for c in p.split('/')]
        for i in range(1, len(parts)): want.add('D ' + '/'.join(parts[:i]))
        want.add('F %s %d %08x' % ('/'.join(parts), len(b), zlib.crc32(b)))
    got = set(l for l in out if l[:2] in ('D ', 'F '))      # rr_moved: hidden
    vol = [l for l in out if l.startswith('V ')]
    days = [int(l[2:]) for l in out if l.startswith('T ')]
    bad = [l for l in out if l.startswith('FAIL')]
    label = 'Alice68 Test'
    ok = not bad and got == want and vol and vol[0].endswith(' ' + label) and days and days[0] > 17000
    expect = {'rr': 'rockridge', 'joliet': 'joliet', 'plain': 'iso9660'}[kind]
    ok = ok and vol[0].split()[1] == expect
    print('%-7s %-9s %3d entries  %s' % (kind, vol[0].split()[1] if vol else '?', len(got), 'OK' if ok else 'FAILED'))
    if not ok:
        fails += 1
        for l in bad: print('   ', l)
        for l in sorted(want - got): print('    missing:', l)
        for l in sorted(got - want): print('    extra:  ', l)
        if vol: print('    ', vol[0], days)
# not an ISO: a FAT image's start
img = os.path.join(T, 'notiso.iso')
open(img, 'wb').write(bytes(65536 * 2))
out = subprocess.run([os.path.join(T, 'iso_test'), img], capture_output=True).stdout.decode()
print('not-iso   %s' % ('OK' if 'iso_open -3' in out else 'FAILED: ' + out.strip()))
fails += 'iso_open -3' not in out
print('ALL OK' if not fails else 'FAILED')
sys.exit(1 if fails else 0)
PY
