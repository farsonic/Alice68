#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Alice68's exFAT partition support (amiga/hdf/exfat.c) on the PC: a card with a FAT32 boot
# partition and an exFAT partition made by mkfs.exfat, filled through exfat-fuse as a PC or Mac
# would fill it (hardfiles and CD images in the root and in HDF / ISO, a fragmented file,
# Latin-1 and mixed-case names, macOS "._" files) plus one 4.5 GB hardfile, and checked with
# fsck.exfat. Needs mkfs.exfat and fsck.exfat (exfatprogs), exfat-fuse, losetup, root.
#   tools/hdf-test/exfat.sh
set -e
P=$(cd "$(dirname "$0")/../.." && pwd); T=${TMPDIR:-/tmp}/alice68-exfat-test
if ! command -v mkfs.exfat >/dev/null || ! command -v mount.exfat-fuse >/dev/null || ! command -v losetup >/dev/null || [ "$(id -u)" != 0 ]; then
    echo "exFAT   SKIPPED: needs root, losetup, mkfs.exfat/fsck.exfat (exfatprogs) and exfat-fuse"; exit 0
fi
rm -rf "$T"; mkdir -p "$T/mnt"
cc -O1 -Wall -Wextra -Werror -std=gnu11 -I"$P/amiga/hdf" -o "$T/exfat_test" "$P/tools/hdf-test/exfat_test.c" "$P/amiga/hdf/exfat.c" "$P/amiga/hdf/fatfs.c" "$P/amiga/hdf/hdf_core.c"
python3 -I - "$T" <<'PY'
import os, random, struct, subprocess, sys, zlib
T = sys.argv[1]
img = T + '/card.img'
SIZE = 6 * 1024 ** 3                                  # sparse: a 6 GB card
P1, P1N = 8192, 131072                                # FAT32 boot partition, 64 MB
P2 = P1 + P1N
P2N = SIZE // 512 - P2
with open(img, 'wb') as f: f.truncate(SIZE)
def run(*a, **k): return subprocess.run(a, check=True, capture_output=True, **k)
run('sfdisk', '-q', img, input=b'label: dos\n%d,%d,c\n%d,%d,7\n' % (P1, P1N, P2, P2N))
def loop(start, n): return run('losetup', '-f', '--show', '-o', str(start * 512), '--sizelimit', str(n * 512), img).stdout.decode().strip()
l1 = loop(P1, P1N); run('mkfs.fat', '-F', '32', l1); run('losetup', '-d', l1)
l2 = loop(P2, P2N); run('mkfs.exfat', '-c', '4096', '-L', 'AmigaStuff', l2)
mnt = T + '/mnt'
run('mount.exfat-fuse', l2, mnt)
rnd = random.Random(68)
def blob(n): return bytes(rnd.getrandbits(8) for _ in range(n))
src = {}
def put(path, data):
    full = os.path.join(mnt, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, 'wb') as f: f.write(data)
    src[path] = data
try:
    put('Workbench.hdf', blob(2 * 1024 * 1024))
    put('My Game.HDF', blob(300 * 1024))
    put('._Workbench.hdf', blob(4096))                # macOS metadata: never a hardfile
    put('Ünïcode ä.hdf', blob(64 * 1024))
    put('Odd size.hdf', blob(1024 * 1024 + 100))      # the tail is ignored
    put('small.hdf', blob(512))                       # too small: ignored
    put('notes.txt', b'not a hardfile\n')
    put('HDF/inner.hdf', blob(128 * 1024))
    put('ISO/cd.iso', blob(700 * 1024))
    put('cd2.ISO', blob(40 * 1024))
    # frag.hdf: written in turns with filler.bin, 4 KB clusters - a FAT chain, not contiguous
    a, b = blob(64 * 4096), blob(64 * 4096)
    with open(os.path.join(mnt, 'HDF/frag.hdf'), 'wb') as fa, open(os.path.join(mnt, 'HDF/filler.bin'), 'wb') as fb:
        for i in range(64):
            fa.write(a[i * 4096:(i + 1) * 4096]); fa.flush(); os.fsync(fa.fileno())
            fb.write(b[i * 4096:(i + 1) * 4096]); fb.flush(); os.fsync(fb.fileno())
    src['HDF/frag.hdf'] = a
    open(os.path.join(mnt, 'big.hdf'), 'wb').close()  # made 4.5 GB below
    # for the folder engine: deep folders, a folder of 400 files (directories over many clusters)
    put('a/b/c/d/e/f/g/h/i/j/deep.txt', b'ten folders down\n')
    put('a/b/c/note \u00e9t\u00e9.txt', blob(5000))
    put('a/b/empty.bin', b'')
    for i in range(400): put('Many/file number %03d with a longish name.dat' % i, blob(rnd.randrange(0, 3000)))
    put('Many/zz last.txt', blob(70000))
finally:
    run('umount', mnt); run('losetup', '-d', l2)

# big.hdf: 4.5 GB, contiguous at the end of the heap (sparse in the image), marker blocks inside
f = open(img, 'r+b')
def rd(sec, n=1): f.seek(sec * 512); return bytearray(f.read(n * 512))
def wr(sec, data): f.seek(sec * 512); f.write(data)
bs = rd(P2)
fat_off, heap_off, count, root = struct.unpack_from('<IIII', bs, 80)[0], struct.unpack_from('<I', bs, 88)[0], struct.unpack_from('<I', bs, 92)[0], struct.unpack_from('<I', bs, 96)[0]
shift = bs[109]; spc = 1 << shift; cbytes = spc * 512
def csec(c): return P2 + heap_off + (c - 2) * spc
rootd = rd(csec(root), spc)                           # the root fits one cluster here
ents = [rootd[i:i + 32] for i in range(0, len(rootd), 32)]
bm = next(e for e in ents if e[0] == 0x81)
bm_first, bm_len = struct.unpack_from('<I', bm, 20)[0], struct.unpack_from('<Q', bm, 24)[0]
bitmap = rd(csec(bm_first), (bm_len + 511) // 512)
BIG = 4608 * 1024 * 1024
need = (BIG + cbytes - 1) // cbytes
first = count + 2 - need - 16
for c in range(first, first + need):
    if bitmap[(c - 2) // 8] >> ((c - 2) % 8) & 1: sys.exit('big.hdf: the heap end is not free')
    bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)
wr(csec(bm_first), bitmap)
for i, e in enumerate(ents):
    if e[0] == 0x85:
        n = e[1] + 1
        name = b''.join(bytes(ents[i + k][2:32]) for k in range(2, n)).decode('utf-16-le')[:ents[i + 1][3]]
        if name == 'big.hdf':
            st = ents[i + 1]
            st[1] = 3                                 # AllocationPossible | NoFatChain
            struct.pack_into('<Q', st, 8, BIG); struct.pack_into('<I', st, 20, first); struct.pack_into('<Q', st, 24, BIG)
            s = 0
            for k in range(n):
                for j, byte in enumerate(ents[i + k]):
                    if k == 0 and j in (2, 3): continue
                    s = ((s >> 1) | ((s & 1) << 15)) + byte & 0xffff
            struct.pack_into('<H', ents[i], 2, s)
            break
else: sys.exit('big.hdf not found')
wr(csec(root), b''.join(ents))
for blk in (0, 8388607, 8388608, BIG // 512 - 1):
    wr(csec(first) + blk, (b'BIG BLOCK %d\0' % blk).ljust(512, b'\0'))
f.close()
open(T + '/src.txt', 'w').write(repr({k: (len(v), zlib.crc32(v[:len(v) // 512 * 512])) for k, v in src.items()}))
open(T + '/srcfull.txt', 'w').write(repr({k: (len(v), zlib.crc32(v)) for k, v in src.items()}))
free = sum(1 for c in range(count) if not bitmap[c // 8] >> (c % 8) & 1)
open(T + '/free.txt', 'w').write('%d %d' % (free, cbytes))
PY
# the image must be a valid exFAT volume (fsck.exfat checks the bitmap, chains, checksums)
L=$(losetup -f --show -o $((139264 * 512)) "$T/card.img")
if ! fsck.exfat -n "$L" > "$T/fsck.txt" 2>&1; then losetup -d "$L"; echo "exFAT   FAILED: fsck.exfat:"; cat "$T/fsck.txt"; exit 1; fi
losetup -d "$L"
"$T/exfat_test" "$T/card.img" NOTES.TXT "$(printf '\334N\317CODE \304.HDF')" missing.hdf > "$T/out.txt" 2> "$T/log.txt" || true
python3 -I - "$T" <<'PY'
import ast, sys
T = sys.argv[1]
src = ast.literal_eval(open(T + '/src.txt').read())
out = open(T + '/out.txt', encoding='latin-1').read().splitlines()
log = open(T + '/log.txt', encoding='latin-1').read()
def amiga(n): return n.split('/')[-1]
want = set()
for p, (n, crc) in src.items():
    name = amiga(p)
    top = p.split('/')[0] if '/' in p else ''
    if top not in ('', 'HDF', 'ISO') or not name.lower().endswith(('.hdf', '.iso')): continue
    if name.startswith('._') or name in ('small.hdf', 'notes.txt', 'filler.bin'): continue
    kind = 'I' if name.lower().endswith('.iso') else 'H'
    pieces = None
    want.add((kind, name, n // 512, crc))
got = set()
fails = []
frag_pieces = 0
for l in out:
    if l[:2] in ('H ', 'I '):
        k, rest = l[0], l[2:]
        name, blocks, pieces, crc = rest.rsplit(' ', 3)
        got.add((k, name, int(blocks), int(crc, 16)))
        if name == 'frag.hdf': frag_pieces = int(pieces)
        if name == 'Workbench.hdf' and pieces != '1': fails.append('Workbench.hdf: %s pieces, want 1 (contiguous)' % pieces)
    elif l.startswith('B '):
        name, blocks, pieces, markers = l[2:].rsplit(' ', 3)
        if (name, blocks, pieces, markers) != ('big.hdf', str(4608 * 2048), '1', '4'): fails.append('big.hdf: ' + l)
    elif l.startswith('FAIL') or l == 'NOEXFAT': fails.append(l)
for x in sorted(want - got): fails.append('missing: %s' % (x,))
for x in sorted(got - want): fails.append('extra: %s' % (x,))
if frag_pieces < 32: fails.append('frag.hdf: %d pieces - not fragmented, the FAT chain is not tested' % frag_pieces)
if 'V AmigaStuff' not in out: fails.append('label: %s' % [l for l in out if l.startswith('V ')])
for n in ('notes.txt', 'Ünïcode ä.hdf', 'HDF', 'ISO', 'big.hdf', 'HDF/frag.hdf', 'HDF/inner.hdf'):
    if 'L ' + n not in out: fails.append('listing: %s missing' % n)
if 'N NOTES.TXT 15' not in out: fails.append('find NOTES.TXT (case)')
if 'N ÜNÏCODE Ä.HDF 65536' not in out: fails.append('find a Latin-1 name in another case')
if 'N missing.hdf -1' not in out: fails.append('find a missing name')
if 'Odd size.hdf: size is not a multiple of 512' not in log: fails.append('odd size not logged')
print('exFAT   card    %d hardfiles/CD images, frag in %d pieces, 4.5 GB file  %s' % (len(got) + 1, frag_pieces, 'OK' if not fails else 'FAILED'))
for x in fails: print('   ', x)
sys.exit(1 if fails else 0)
PY
# the folder engine: every entry through the ffs_* calls, compared with what was put there
"$T/exfat_test" -w "$T/card.img" > "$T/walk.txt" 2> "$T/walklog.txt" || true
python3 -I - "$T" <<'PY'
import ast, sys
T = sys.argv[1]
src = ast.literal_eval(open(T + '/srcfull.txt').read())
out = open(T + '/walk.txt', encoding='latin-1').read().splitlines()
want = set()
for p, (n, crc) in src.items():
    parts = p.split('/')
    for i in range(1, len(parts)): want.add('D ' + '/'.join(parts[:i]))
    want.add('F %s %d %08x' % (p, n, crc))
want.add('F big.hdf %d -' % (4608 * 1024 * 1024)); want.add('F HDF/filler.bin %d' % (64 * 4096))
got = set(l for l in out if l[:2] in ('D ', 'F '))
got = set(' '.join(l.split(' ')[:-1]) if l.startswith('F HDF/filler.bin') else l for l in got)
fails = [l for l in out if l.startswith('FAIL') or l == 'NOEXFAT']
fails += ['missing: ' + l for l in sorted(want - got)] + ['extra: ' + l for l in sorted(got - want)]
s = [l for l in out if l.startswith('S ')]
if not s or s[0][2:] != open(T + '/free.txt').read(): fails.append('free clusters %s, want %s' % (s, open(T + '/free.txt').read()))
print('exFAT   folders %d entries through ffs_*, free space, writes refused  %s' % (len(got), 'OK' if not fails else 'FAILED'))
for x in fails[:20]: print('   ', x)
sys.exit(1 if fails else 0)
PY
# writing: a sequence of changes through the ffs_* calls, then fsck.exfat, then exfat-fuse reads it all back
OFF=$((139264 * 512))
cp --sparse=always "$T/card.img" "$T/w.img"
"$T/exfat_test" -W "$T/w.img" > "$T/wout.txt" 2> "$T/wlog.txt" || true
L=$(losetup -f --show -o $OFF "$T/w.img")
fsck.exfat -n "$L" > "$T/wfsck.txt" 2>&1 && FS=clean || FS=dirty
mount.exfat-fuse -o ro "$L" "$T/mnt" 2>/dev/null
python3 -I - "$T" "$FS" <<'PY'
import ast, os, sys, zlib
T, fs = sys.argv[1], sys.argv[2]
mnt = T + '/mnt'
out = open(T + '/wout.txt', encoding='latin-1').read().splitlines()
fails = [l for l in out if l.startswith('FAIL') or l == 'NOEXFAT']
if fs != 'clean': fails.append('fsck.exfat: ' + open(T + '/wfsck.txt').read())
def host(p): return os.path.join(mnt, p.encode('latin-1').decode('latin-1'))
made = {}
for l in out:
    if l.startswith('E '):
        path, size, crc = l[2:].rsplit(' ', 2); made[path] = (int(size), int(crc, 16))
        try: b = open(host(path), 'rb').read()
        except OSError as e: fails.append('%s: %s' % (path, e)); continue
        if (len(b), zlib.crc32(b)) != made[path]: fails.append('%s: %d %08x on the card, the engine read %s' % (path, len(b), zlib.crc32(b), made[path]))
    elif l.startswith('X '):
        if os.path.exists(host(l[2:])): fails.append('%s: still there' % l[2:])
src = ast.literal_eval(open(T + '/srcfull.txt').read())
for p, (n, crc) in src.items():                      # what the PC put there and the test did not touch
    if p in made or p.startswith('New Dir'): continue
    b = open(host(p), 'rb').read()
    if (len(b), zlib.crc32(b)) != (n, crc): fails.append('%s: changed' % p)
st = os.statvfs(mnt)
free = st.f_bfree * st.f_frsize // int(open(T + '/free.txt').read().split()[1])
want = [int(l[2:]) for l in out if l.startswith('S ')]
if want != [free]: fails.append('free clusters: the engine says %s, the card %d' % (want, free))
writes = [l for l in out if l.startswith('DONE')]
print('exFAT   writes  %d files made/changed, %d removed, %s, fsck.exfat %s  %s' % (len(made), sum(1 for l in out if l.startswith('X ')), writes[0] if writes else '?', fs, 'OK' if not fails else 'FAILED'))
for x in fails[:20]: print('   ', x)
sys.exit(1 if fails else 0)
PY
umount "$T/mnt"; losetup -d "$L"
# power cuts: the same sequence, the card "switched off" after N sector writes (N spread over the
# whole run); Alice68's validator (xfat_check) must repair every one so that fsck.exfat finds it
# clean and the volume is no longer marked dirty
TOTAL=$(sed -n 's/^DONE //p' "$T/wout.txt")
python3 -I - "$T" "$TOTAL" "$OFF" <<'PY'
import subprocess, sys
T, total, off = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
budgets = sorted(set([0, 1, 2, 3, total - 1] + [total * i // 37 for i in range(1, 37)]))
clean = repaired = 0
fails = []
for b in budgets:
    img = T + '/cut.img'
    subprocess.run(['cp', '--sparse=always', T + '/card.img', img], check=True)
    subprocess.run([T + '/exfat_test', '-W', img, str(b)], capture_output=True)
    c = subprocess.run([T + '/exfat_test', '-C', img], capture_output=True, text=True).stdout.split()
    if c[:2] != ['C', 'ok']: fails.append('cut after %d writes: the validator: %s' % (b, ' '.join(c))); continue
    if any(int(v) for v in c[4:]): repaired += 1
    else: clean += 1
    with open(img, 'rb') as f: f.seek(off + 106); flags = int.from_bytes(f.read(2), 'little')
    if flags & 2: fails.append('cut after %d writes: still marked dirty' % b)
    L = subprocess.run(['losetup', '-f', '--show', '-o', str(off), img], capture_output=True, text=True, check=True).stdout.strip()
    try:
        n = subprocess.run(['fsck.exfat', '-n', L], capture_output=True, text=True)
        if n.returncode != 0: fails.append('cut after %d writes, validated (%s): fsck.exfat:\n%s' % (b, ' '.join(c), n.stdout[-600:]))
    finally:
        subprocess.run(['losetup', '-d', L])
print('exFAT   cuts    %d power cuts over %d writes, each validated on "the Amiga": %d already consistent, %d repaired; fsck.exfat clean  %s' % (len(budgets), total, clean, repaired, 'OK' if not fails else 'FAILED'))
for f in fails[:5]: print('   ', f)
sys.exit(1 if fails else 0)
PY
# the validator against damage made by hand: what it can fix (all at once), then what it must not touch
python3 -I - "$T" "$OFF" <<'PY'
import hashlib, os, struct, subprocess, sys, zlib
T, off = sys.argv[1], int(sys.argv[2])
img = T + '/dmg.img'
subprocess.run(['cp', '--sparse=always', T + '/w.img', img], check=True)    # the written card: clean
f = open(img, 'r+b')
def rd(sec, n=1): f.seek(off + sec * 512); return bytearray(f.read(n * 512))
def wr(sec, b): f.seek(off + sec * 512); f.write(b)
bs = rd(0)
fat_off, heap, count, root = struct.unpack_from('<I', bs, 80)[0], struct.unpack_from('<I', bs, 88)[0], struct.unpack_from('<I', bs, 92)[0], struct.unpack_from('<I', bs, 96)[0]
spc = 1 << bs[109]
def fat(c): f.seek(off + (fat_off + c // 128) * 512 + c % 128 * 4); return struct.unpack('<I', f.read(4))[0]
def setfat(c, v): f.seek(off + (fat_off + c // 128) * 512 + c % 128 * 4); f.write(struct.pack('<I', v))
def csec(c): return heap + (c - 2) * spc
def chain(first, nofat, size):
    out, c = [], first
    while c and len(out) < max(1, -(-size // (spc * 512))) if size else c and c < 0xfffffff7:
        out.append(c); c = c + 1 if nofat else fat(c)
        if not size and len(out) > count: break
    return out
def dir_ents(first, nofat, size):
    cl = chain(first, nofat, size)
    raw = b''.join(bytes(rd(csec(c), spc)) for c in cl)
    return cl, raw
def sets(first, nofat, size):
    cl, raw = dir_ents(first, nofat, size)
    i, out = 0, {}
    while i * 32 < len(raw):
        e = raw[i * 32:i * 32 + 32]
        if e[0] == 0: break
        if e[0] == 0x85:
            n = e[1] + 1; st = raw[(i + 1) * 32:(i + 2) * 32]
            name = b''.join(raw[(i + k) * 32 + 2:(i + k) * 32 + 32] for k in range(2, n)).decode('utf-16-le')[:st[3]]
            out[name] = (i, n, struct.unpack_from('<I', st, 20)[0], st[1] & 2 != 0, struct.unpack_from('<Q', st, 24)[0])
            i += n
        else: i += 1
    return cl, out
def ent_pos(cl, i): return csec(cl[i * 32 // (spc * 512)]) + (i * 32 % (spc * 512)) // 512, i * 32 % 512
rcl, rs = sets(root, False, 0)
bm = next(e for e in [bytes(dir_ents(root, False, 0)[1][k:k + 32]) for k in range(0, 4096, 32)] if e[0] == 0x81)
bm_first = struct.unpack_from('<I', bm, 20)[0]
def bmbit(c, on):
    i = c - 2; s = csec(bm_first) + i // 8 // 512; b = rd(s); o = i // 8 % 512
    b[o] = b[o] | (1 << (i % 8)) if on else b[o] & ~(1 << (i % 8)) & 0xff; wr(s, b)
nd = rs['New Dir']; ncl, ns = sets(nd[2], nd[3], nd[4])
# 1. a leaked cluster: marked used, nobody's
bmbit(count, 1)
# 2. a used cluster marked free: the first of "turn A"
ta = ns['turn A']; bmbit(ta[2], 0)
# 3. a wrong checksum: a set's modify time changed without it
sec, o = ent_pos(ncl, ns['empty'][0]); b = rd(sec); b[o + 12] ^= 0x01; wr(sec, b)
# 4. a chain longer than its file: "turn A" (a FAT chain) given one more cluster
tchain = chain(ta[2], ta[3], ta[4]); extra = count - 1; setfat(tchain[-1], extra); setfat(extra, 0xffffffff); bmbit(extra, 1)
# 5. a duplicate set (a move cut off): "a/b/c/note été.txt"'s set copied into free entries of the root
a = rs['a']; acl, a_s = sets(a[2], a[3], a[4]); b_ = a_s['b']; bcl, b_s = sets(b_[2], b_[3], b_[4]); c_ = b_s['c']; ccl, c_s = sets(c_[2], c_[3], c_[4])
nt = c_s['note \u00e9t\u00e9.txt']; craw = dir_ents(c_[2], c_[3], c_[4])[1]; dup = craw[nt[0] * 32:(nt[0] + nt[1]) * 32]
rraw = dir_ents(root, False, 0)[1]; free_at = next(i for i in range(len(rraw) // 32) if rraw[i * 32] == 0)
for k in range(nt[1]):
    sec, o = ent_pos(rcl, free_at + k); b = rd(sec); b[o:o + 32] = dup[k * 32:k * 32 + 32]; wr(sec, b)
f.close()
fails = []
c = subprocess.run([T + '/exfat_test', '-C', img], capture_output=True, text=True).stdout.split()
want = {'sums': 1, 'dropped': 1, 'trimmed': 1, 'freed': 2, 'marked': 1}
got = dict(zip(['dirs', 'files', 'sums', 'dropped', 'trimmed', 'shortened', 'freed', 'marked'], [int(v) for v in c[2:]])) if c[:2] == ['C', 'ok'] else None
if not got or any(got[k] != v for k, v in want.items()): fails.append('validator: %s, want %s' % (' '.join(c), want))
L = subprocess.run(['losetup', '-f', '--show', '-o', str(off), img], capture_output=True, text=True, check=True).stdout.strip()
fs = subprocess.run(['fsck.exfat', '-n', L], capture_output=True, text=True)
if fs.returncode: fails.append('fsck.exfat after the repair:\n' + fs.stdout[-800:])
os.makedirs(T + '/dmnt', exist_ok=True)
subprocess.run(['mount.exfat-fuse', '-o', 'ro', L, T + '/dmnt'], check=True, capture_output=True)
try:
    note = 'a/b/c/note \u00e9t\u00e9.txt'
    for l in open(T + '/wout.txt', encoding='latin-1').read().splitlines():
        if l.startswith('E '):
            path, size, crc = l[2:].rsplit(' ', 2)
            if path == note:                                 # the duplicate: one of the two stays (either is right)
                there = [q for q in (note, 'note \u00e9t\u00e9.txt') if os.path.exists(os.path.join(T + '/dmnt', q))]
                if len(there) != 1: fails.append('the moved file: %d copies' % len(there)); continue
                path = there[0]
            b = open(os.path.join(T + '/dmnt', path), 'rb').read()
            if (len(b), '%08x' % zlib.crc32(b)) != (int(size), crc): fails.append('%s changed by the repair' % path)
finally:
    subprocess.run(['umount', T + '/dmnt']); subprocess.run(['losetup', '-d', L])
# what it must not touch: two different files sharing a cluster
img2 = T + '/dmg2.img'
subprocess.run(['cp', '--sparse=always', T + '/w.img', img2], check=True)
f = open(img2, 'r+b')
tb = ns['Gr\u00fc\u00dfe \u00e4\u00f6\u00ff.txt'] if 'Gr\u00fc\u00dfe \u00e4\u00f6\u00ff.txt' in ns else None
la = rs['Lots']; lcl, ls_ = sets(la[2], la[3], la[4]); bigm = ls_['big moved.bin']
setfat(tchain[0], bigm[2] + 1) if not ta[3] else None
f.close()
before = hashlib.md5(open(img2, 'rb').read()).hexdigest()
c2 = subprocess.run([T + '/exfat_test', '-C', img2], capture_output=True, text=True).stdout.strip()
if not c2.startswith('C bad') or hashlib.md5(open(img2, 'rb').read()).hexdigest() != before:
    fails.append('two files sharing a cluster: %s, the image %s' % (c2, 'unchanged' if hashlib.md5(open(img2, 'rb').read()).hexdigest() == before else 'CHANGED'))
print('exFAT   damage  fixed: %s; refused (image untouched): %s  %s' % (' '.join(c[2:]) if c else '?', c2[6:] if c2.startswith('C bad') else c2, 'OK' if not fails else 'FAILED'))
for x in fails[:10]: print('   ', x)
sys.exit(1 if fails else 0)
PY
# a broken boot checksum: the partition is refused, with a reason
cp --sparse=always "$T/card.img" "$T/bad.img"
printf 'X' | dd of="$T/bad.img" bs=1 seek=$((139264 * 512 + 120)) conv=notrunc 2>/dev/null
"$T/exfat_test" "$T/bad.img" > "$T/bad.txt" 2> "$T/badlog.txt" || true
if grep -q NOEXFAT "$T/bad.txt" && grep -q "boot checksum wrong" "$T/badlog.txt"; then echo "exFAT   broken  boot checksum refused  OK"; else echo "exFAT   broken  FAILED"; cat "$T/bad.txt" "$T/badlog.txt"; exit 1; fi
echo "exFAT   ALL OK"
