#!/usr/bin/env python3
"""alice68hdf.device's folders (amiga/hdf/folders.c) on a 68k emulator
(machine68k / Musashi): a FAT32 card made by mkfs.fat and mtools with a
config.txt, the ALICE68 folder, other folders and a mounted hardfile; a fake
exec, expansion.library, dos.library and brcm-emmc.device.

    harness_folders.py DEVICE.hunk DEVICE.elf WORKDIR [cardro]

Checks the boot nodes the device makes for the folders, then starts a
handler (fold_start) and sends it DOS packets (fold_do): directory listing,
reading, writing, locks, renaming, deleting, the mounted hardfile kept
read-only. At the end mtools must read back what the packets wrote and
fsck.fat must find nothing wrong with the card.
SPDX-License-Identifier: GPL-2.0-only"""
import os, random, struct, subprocess, sys
import machine68k as M

R = M.Register
mach = M.Machine(M.CPUType.M68020, 16384)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYSBASE, EXPBASE, EMMC, DOSBASE = 0x8000, 0xA000, 0xC000, 0xE000
LOAD, DEVBASE, CONFIGDEV, PORT = 0x20000, 0x70000, 0x7F000, 0x7F800
HEAP, HEAP_END = 0x400000, 0xC00000
STACK = 0x200000
hunk, elf, work = sys.argv[1:4]
card_ro = 'cardro' in sys.argv[4:]
env = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8')

def die(msg):
    print('FAIL:', msg); sys.exit(1)

# ---- the card ----------------------------------------------------------------------
img = os.path.join(work, 'folders.img')
src = os.path.join(work, 'folders-src')
subprocess.run(['rm', '-rf', src, img], check=True)
rnd = random.Random(1985)
def blob(n): return bytes(rnd.getrandbits(8) for _ in range(n))
files = {
    'ALICE68/C/Alice68-HDF': blob(1776),
    'ALICE68/Install': b'; Alice68 installer\n',
    'Games/Long File Name.txt': blob(70000),
    'Games/readme.txt': b'hello from the PC\n',
    'Games/Sub/deep.bin': blob(3000),
    'Work/Docs/letter.txt': b'Dear Amiga,\n',
}
raw = bytearray(2048 * 512)                          # a single-partition hardfile (DOS\1, WinUAE geometry)
raw[0:4] = b'DOS\x01'
struct.pack_into('>I', raw, 1024 * 512, 2); struct.pack_into('>I', raw, 1024 * 512 + 508, 1)
raw[1024 * 512 + 432] = 6; raw[1024 * 512 + 433:1024 * 512 + 439] = b'RawVol'
files['HDF/raw.hdf'] = bytes(raw)
for p, b in files.items():
    os.makedirs(os.path.dirname(os.path.join(src, p)), exist_ok=True)
    open(os.path.join(src, p), 'wb').write(b)
config = ('# the Pi\'s settings\n[pi4]\narm_64bit=1\n[all]\n'
          'alice68.folder=Games\r\n'
          'alice68.folder=/Work/Docs:Documents\n'
          'alice68.folder=HDF:Hardfiles\n'
          'alice68.folder=NoSuchFolder\n'
          'alice68.folder=Games/readme.txt\n'
          '  alice68.folder=ALICE68  \n')
open(os.path.join(src, 'config.txt'), 'w').write(config)
subprocess.run(['mkfs.fat', '-F', '32', '-s', '1', '-C', img, '40000'], check=True, stdout=subprocess.DEVNULL)
for top in ('ALICE68', 'Games', 'Work', 'HDF', 'config.txt'):
    subprocess.run(['mcopy', '-s', '-i', img, os.path.join(src, top), '::/'], check=True, env=env)
card = bytearray(open(img, 'rb').read())

# ---- the fake system ------------------------------------------------------------------
done = {'flag': False}
exc = {'hit': None}
def t_exit(op, pc): done['flag'] = True; cpu.end()
EXIT_ADDR = 0x1000
mem.w16(EXIT_ADDR, 0xA000 | traps.setup(t_exit))
def t_exc(op, pc):
    sp = cpu.r_reg(R.A7); exc['hit'] = (mem.r16(sp + 6) & 0xfff) // 4, mem.r32(sp + 2); done['flag'] = True; cpu.end()
EXC_ADDR = 0x1010
mem.w16(EXC_ADDR, 0xA000 | traps.setup(t_exc))
for v in range(2, 64): mem.w32(v * 4, EXC_ADDR)
mem.w32(4, SYSBASE)
known = set()
def lvo(base, off, fn):
    known.add((base, off)); mem.w16(base + off, 0xA000 | traps.setup(fn, auto_rts=True))
def d(r): return cpu.r_reg(r)
def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)
def t_unknown(op, pc): die('library call to an LVO the harness does not have, at $%x' % pc)
UNK = traps.setup(t_unknown, auto_rts=True)
for base in (SYSBASE, EXPBASE, DOSBASE):
    for off in range(6, 1000, 6): mem.w16(base - off, 0xA000 | UNK)
heap = {'p': HEAP}
allocs = {}
def alloc(n):
    a = (heap['p'] + 7) & ~7; heap['p'] = a + n
    if heap['p'] > HEAP_END: die('heap exhausted')
    mem.w_block(a, bytes(n)); allocs[a] = n
    return a
def freemem(o, p):
    a, n = d(R.A1), d(R.D0)
    if a not in allocs: die('FreeMem of %x, never allocated' % a)
    if allocs[a] != n: die('FreeMem(%x, %d): allocated %d' % (a, n, allocs[a]))
    del allocs[a]
lvo(SYSBASE, -198, lambda o, p: setd0(alloc(d(R.D0))))
lvo(SYSBASE, -210, freemem)
for off in (-132, -138, -558, -564, -570, -636, -414, -672, -660, -450): lvo(SYSBASE, off, lambda o, p: None)
def newlist(l): mem.w32(l, l + 4); mem.w32(l + 4, 0); mem.w32(l + 8, l)
def addtail(l, n):
    pred = mem.r32(l + 8); mem.w32(n, l + 4); mem.w32(n + 4, pred); mem.w32(pred, n); mem.w32(l + 8, n)
serial = []
lvo(SYSBASE, -516, lambda o, p: serial.append(chr(d(R.D0) & 0xff)))
def createport(o, p):
    a = alloc(34); mem.w8(a + 8, 4); newlist(a + 20); setd0(a)
lvo(SYSBASE, -666, createport)
def createio(o, p):
    a = alloc(d(R.D0)); mem.w32(a + 14, d(R.A0)); setd0(a)
lvo(SYSBASE, -654, createio)
lvo(SYSBASE, -552, lambda o, p: setd0({'expansion.library': EXPBASE, 'dos.library': DOSBASE}.get(mem.r_cstr(d(R.A1)), 0)))
lvo(SYSBASE, -498, lambda o, p: setd0(0))            # no FileSystem.resource
lvo(SYSBASE, -330, lambda o, p: setd0(16))
lvo(SYSBASE, -282, lambda o, p: None)                # AddTask (the hardfile I/O task)
def openDev(o, p):
    name, unit, io = mem.r_cstr(d(R.A0)), d(R.D0), d(R.A1)
    ok = name == 'brcm-emmc.device' and unit == 0
    if ok: mem.w32(io + 20, EMMC)
    mem.w8(io + 31, 0 if ok else 0xff); setd0(0 if ok else 0xffffffff)
lvo(SYSBASE, -444, openDev)
stats = {'reads': 0, 'writes': 0}
def doio(o, p):
    io = d(R.A1)
    cmd, data, length, off = mem.r16(io + 28), mem.r32(io + 40), mem.r32(io + 36), mem.r32(io + 44)
    err = 0
    if cmd == 15: mem.w32(io + 32, 1 if card_ro else 0)
    elif cmd == 0x4000: err = -3
    elif cmd == 24: err = -3
    elif cmd == 3:
        if card_ro: err = 28
        elif off % 512 or length % 512 or off + length > len(card): err = -4
        else: card[off:off + length] = bytes(mem.r_block(data, length)); mem.w32(io + 32, length); stats['writes'] += 1
    elif cmd == 2:
        if off % 512 or length % 512 or off + length > len(card): err = -4
        else: mem.w_block(data, bytes(card[off:off + length])); mem.w32(io + 32, length); stats['reads'] += 1
    else: err = -3
    mem.w8(io + 31, err & 0xff); setd0(err & 0xffffffff)
lvo(SYSBASE, -456, doio)
# expansion.library
newlist(EXPBASE + 74)
def bstr_at(a, s): mem.w8(a, len(s)); mem.w_block(a + 1, s.encode('latin1'))
def bstr(s):
    a = (alloc(len(s) + 8) + 3) & ~3; bstr_at(a, s); return a >> 2
def rbstr(b):
    a = b << 2; return bytes(mem.r_block(a + 1, mem.r8(a))).decode('latin1')
lvo(EXPBASE, -72, lambda o, p: setd0(CONFIGDEV if d(R.D0) == 0x6d73 else 0))
dosnodes = {}
def makedosnode(o, p):
    pkt = d(R.A0)
    ts = mem.r32(pkt + 16)
    dn = alloc(44); mem.w32(dn + 40, bstr(mem.r_cstr(mem.r32(pkt))))
    fssm = alloc(16); mem.w32(fssm, mem.r32(pkt + 8)); mem.w32(dn + 28, fssm >> 2)
    dosnodes[dn] = dict(name=mem.r_cstr(mem.r32(pkt)), dev=mem.r_cstr(mem.r32(pkt + 4)), unit=mem.r32(pkt + 8),
                        env=[mem.r32(pkt + 16 + 4 * i) for i in range(ts + 1)])
    setd0(dn)
lvo(EXPBASE, -144, makedosnode)
boots = []
def addbootnode(o, p):
    pri, fl, dn = d(R.D0), d(R.D1), d(R.A0)
    pri = pri - (1 << 32) if pri & 0x80000000 else pri
    info = dict(dosnodes[dn]); info.update(pri=pri, flags=fl, dn=dn)
    boots.append(info)
    bn = alloc(20); mem.w8(bn + 9, pri & 0xff); mem.w32(bn + 16, dn); addtail(EXPBASE + 74, bn)
    setd0(1)
lvo(EXPBASE, -36, addbootnode)
# dos.library
lvo(DOSBASE, -192, lambda o, p: (mem.w32(d(R.D1), 17841), mem.w32(d(R.D1) + 4, 13 * 60 + 37), mem.w32(d(R.D1) + 8, 2100)))
dl_locks = []
lvo(DOSBASE, -654, lambda o, p: (dl_locks.append(d(R.D1)), setd0(0x1234)))
lvo(DOSBASE, -660, lambda o, p: dl_locks.append(-d(R.D1)))
doslist = []
lvo(DOSBASE, -678, lambda o, p: (doslist.append(d(R.D1)), setd0(1)))

# ---- load and start -----------------------------------------------------------------------
h = open(hunk, 'rb').read()
w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
n = w(28) * 4
code = bytearray(h[32:32 + n]); q = 32 + n
if w(q) == 0x3ec:
    for i in range(w(q + 4)):
        r = w(q + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
mem.w_block(LOAD, bytes(code))
syms = {}
for l in subprocess.run(['m68k-linux-gnu-nm', elf], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3: syms[f[2]] = int(f[0], 16) + LOAD
def call(addr, regs=None, args=(), name=''):
    done['flag'] = False
    sp = STACK - 256
    mem.w32(sp, EXIT_ADDR)
    for i, a in enumerate(args): mem.w32(sp + 4 + 4 * i, a)
    cpu.w_reg(R.A7, sp)
    for r_, v in (regs or {}).items(): cpu.w_reg(r_, v & 0xffffffff)
    cpu.w_pc(addr); cpu.w_sr(0)
    k = 0
    while not done['flag'] and k < 20000: cpu.execute(100000); k += 1
    if exc['hit']: die('%s: CPU exception %d at $%x (hunk offset $%x)' % (name, exc['hit'][0], exc['hit'][1], exc['hit'][1] - LOAD))
    if not done['flag']: die('%s: did not return' % name)
    return cpu.r_reg(R.D0)
rt = LOAD + 4
inittab = mem.r32(rt + 22)
initfn = mem.r32(inittab + 12)
if call(initfn, {R.D0: DEVBASE, R.A0: 0, R.A6: SYSBASE}, name='Init') != DEVBASE: die('Init')
wr = mem.r32(DEVBASE + 44 + 4 * 16)
logtxt = bytes(mem.r_block(DEVBASE + 48 + 4 * 16, min(wr, 4096))).decode('latin1')
print('--- driver log ---\n' + logtxt.rstrip())

# ---- the boot nodes -----------------------------------------------------------------------------
fold = [b for b in boots if b['dev'] == 'alice68hdf.device' and b['unit'] >= 0x4000]
got = [(b['name'], b['unit'] - 0x4000, b['pri']) for b in fold]
want = [('SDF0', 0, -128), ('SDF1', 1, -128), ('SDF2', 2, -128), ('SDF3', 3, -128)]
if got != want: die('folder boot nodes %s, want %s' % (got, want))
for b in fold:
    dn = b['dn']
    seg, gv, stack = mem.r32(dn + 32), mem.r32(dn + 36), mem.r32(dn + 20)
    if b['flags'] != 1 or gv != 0xffffffff or stack < 8192 or b['env'][16] != 0x46415401 or b['env'][15] != 0xffffff80:
        die('%s: flags %d globvec %x stack %d env %s' % (b['name'], b['flags'], gv, stack, b['env']))
    c = (seg << 2) + 4
    if (mem.r16(c), mem.r32(c + 2), mem.r16(c + 6), mem.r32(c + 8), mem.r16(c + 12), mem.r16(c + 14)) != \
       (0x4879, DEVBASE, 0x4eb9, syms['folder_main'], 0x588f, 0x4e75):
        die('the seglist does not call folder_main(db)')
for line in ('folder ALICE68: volume ALICE68 (SDF0:)', 'folder Games: volume Games (SDF1:)',
             'folder Work/Docs: volume Documents (SDF2:)', 'folder HDF: volume Hardfiles (SDF3:)',
             'alice68.folder=NoSuchFolder: no such folder', 'alice68.folder=Games/readme.txt: not a folder'):
    if line not in logtxt: die('log has no "%s"' % line)
if 'raw.hdf' not in logtxt or not any(b['name'] == 'DH0' for b in boots): die('the hardfile in HDF/ was not mounted')
print('boot nodes: %s, none bootable, seglist -> folder_main' % ', '.join('%s' % g[0] for g in got))

# ---- a handler and its packets ---------------------------------------------------------------------
def start(unit):
    x = call(syms['fold_start'], args=(DEVBASE, 0x4000 + unit, PORT), name='fold_start')
    if not x: die('fold_start %d failed' % unit)
    return x
G = start(1)
dl = doslist[-1]
if mem.r32(dl + 4) != 2 or mem.r32(dl + 8) != PORT or rbstr(mem.r32(dl + 40)) != 'Games' or mem.r32(dl + 32) != 0x444f5300:
    die('volume node: type %d task %x name %s' % (mem.r32(dl + 4), mem.r32(dl + 8), rbstr(mem.r32(dl + 40))))
if dl_locks[-2:] != [10, -10]: die('AddDosEntry without LockDosList(LDF_VOLUMES|LDF_WRITE): %s' % dl_locks)
PKT = alloc(48)
if card_ro:                                          # unit0=ro: everything reads, nothing writes
    f_ = (alloc(48) + 3) & ~3
    mem.w_block(PKT, bytes(48)); mem.w32(PKT + 8, 1006); mem.w32(PKT + 20, f_ >> 2); mem.w32(PKT + 28, bstr('new.txt'))
    call(syms['fold_do'], args=(G, PKT), name='FINDOUTPUT')
    if mem.r32(PKT + 12) != 0 or mem.r32(PKT + 16) != 214: die('read-only card: MODE_NEWFILE answered %d/%d' % (mem.r32(PKT + 12), mem.r32(PKT + 16)))
    mem.w_block(PKT, bytes(48)); mem.w32(PKT + 8, 16); mem.w32(PKT + 24, bstr('readme.txt'))
    call(syms['fold_do'], args=(G, PKT), name='DELETE')
    if mem.r32(PKT + 16) != 214: die('read-only card: delete answered %d' % mem.r32(PKT + 16))
    mem.w_block(PKT, bytes(48)); mem.w32(PKT + 8, 1005); mem.w32(PKT + 20, f_ >> 2); mem.w32(PKT + 28, bstr('readme.txt'))
    call(syms['fold_do'], args=(G, PKT), name='FINDINPUT')
    if mem.r32(PKT + 12) == 0: die('read-only card: cannot read')
    if 'read-only' not in logtxt or bytes(card) != open(img, 'rb').read(): die('read-only card changed')
    print('read-only card: reads, refuses writes (214), card unchanged'); print('ALL OK'); sys.exit(0)
BUF = alloc(0x40000)
FIB = alloc(260 + 4) & ~3 | 0; FIB = (FIB + 3) & ~3
INFO = (alloc(40) + 3) & ~3
names = {}
def name(s):
    if s not in names: names[s] = bstr(s)
    return names[s]
def pkt(ctx, typ, *args):
    mem.w_block(PKT, bytes(48))
    mem.w32(PKT + 8, typ)
    for i, a in enumerate(args): mem.w32(PKT + 20 + 4 * i, a & 0xffffffff)
    call(syms['fold_do'], args=(ctx, PKT), name='packet %d' % typ)
    r1, r2 = mem.r32(PKT + 12), mem.r32(PKT + 16)
    return (r1 - (1 << 32) if r1 & 0x80000000 else r1), r2
def ok(res, what):
    if res[0] == 0: die('%s: failed, error %d' % (what, res[1]))
    return res[0]
def err(res, code, what):
    if res[0] not in (0, -1) or res[1] != code: die('%s: got %s, want error %d' % (what, res, code))
checks = [0]
def chk(c, what):
    if not c: die(what)
    checks[0] += 1

LOCATE, FREE, DELETE, RENAME, COPYDIR, PROTECT, MKDIR, EXAMINE, EXNEXT, DISKINFO, INFOP, PARENT, SAMELOCK = 8, 15, 16, 17, 19, 21, 22, 23, 24, 25, 26, 29, 40
FINDUPDATE, FINDINPUT, FINDOUTPUT, END, SEEK, SETSIZE, EXFH, ISFS, CURVOL = 1004, 1005, 1006, 1007, 1008, 1022, 1034, 1027, 7
def fib():
    return dict(key=mem.r32(FIB), type=mem.r32(FIB + 4) - (1 << 32 if mem.r32(FIB + 4) & 0x80000000 else 0),
                name=bytes(mem.r_block(FIB + 9, mem.r8(FIB + 8))).decode('latin1'), prot=mem.r32(FIB + 116),
                size=mem.r32(FIB + 124), days=mem.r32(FIB + 132), mins=mem.r32(FIB + 136))
def listdir(ctx, lock):
    ok(pkt(ctx, EXAMINE, lock, FIB >> 2), 'Examine')
    out = []
    while True:
        r = pkt(ctx, EXNEXT, lock, FIB >> 2)
        if r[0] == 0:
            if r[1] != 232: die('ExNext error %d' % r[1])
            return out
        out.append(fib())
def fh():
    a = (alloc(48) + 3) & ~3; return a
def openf(ctx, typ, path, lock=0):
    f = fh(); r = pkt(ctx, typ, f >> 2, lock, name(path))
    return (mem.r32(f + 36) if r[0] else 0), r
def readall(ctx, h, chunk):
    out = b''
    while True:
        n = pkt(ctx, ord('R'), h, BUF, chunk)[0]
        if n < 0: die('Read error')
        if n == 0: return out
        out += bytes(mem.r_block(BUF, n))
def write(ctx, h, data):
    mem.w_block(BUF, data); return pkt(ctx, ord('W'), h, BUF, len(data))

root = ok(pkt(G, LOCATE, 0, name(''), -2), 'Lock root')
ok(pkt(G, EXAMINE, root, FIB >> 2), 'Examine root')
f0 = fib()
chk(f0['type'] == 1 and f0['name'] == 'Games', 'root: %s' % f0)
chk(pkt(G, PARENT, root)[0] == 0 and pkt(G, PARENT, root)[1] == 0, 'the root has no parent')
ls = {e['name']: e for e in listdir(G, root)}
chk(set(ls) == {'Long File Name.txt', 'readme.txt', 'Sub'}, 'Games lists %s' % sorted(ls))
chk(ls['Sub']['type'] == 2 and ls['readme.txt']['type'] == -3 and ls['Long File Name.txt']['size'] == 70000, 'entry types and sizes')
chk(ls['readme.txt']['prot'] & 0xf == 0, 'rwed allowed: %x' % ls['readme.txt']['prot'])
h1, r = openf(G, FINDINPUT, 'Long File Name.txt')
chk(h1 and readall(G, h1, 4093) == files['Games/Long File Name.txt'], 'read a file in odd pieces')
chk(pkt(G, SEEK, h1, 1000, -1)[0] == 70000, 'Seek returns the old position')
n = pkt(G, ord('R'), h1, BUF, 10)[0]
chk(n == 10 and bytes(mem.r_block(BUF, 10)) == files['Games/Long File Name.txt'][1000:1010], 'read after Seek')
err(pkt(G, SEEK, h1, 70001, -1), 219, 'Seek past the end')
chk(pkt(G, SEEK, h1, -5, 1)[0] == 1010 and pkt(G, SEEK, h1, 0, 0)[0] == 69995, 'Seek from the end')
ok(pkt(G, END, h1), 'Close')
h2, r = openf(G, FINDINPUT, 'Games:sub/DEEP.BIN')
chk(h2 and readall(G, h2, 512) == files['Games/Sub/deep.bin'], 'volume prefix, any case, a sub-folder')
ok(pkt(G, END, h2), 'Close')
err(openf(G, FINDINPUT, 'nothing.here')[1], 205, 'open a file that is not there')
err(openf(G, FINDINPUT, 'Sub')[1], 212, 'open a folder as a file')
err(openf(G, FINDINPUT, 'Nope/x')[1], 204, 'a path through a missing folder')

# writing
data = blob(100000)
h3, r = openf(G, FINDOUTPUT, 'Made on the Amiga \xe4.txt')
chk(h3 != 0, 'MODE_NEWFILE: %s' % (r,))
for i in range(0, len(data), 7777): chk(write(G, h3, data[i:i + 7777])[0] == len(data[i:i + 7777]), 'Write')
err(openf(G, FINDINPUT, 'Made on the Amiga \xe4.txt')[1], 202, 'a file open for writing is exclusive')
ok(pkt(G, END, h3), 'Close')
ok(pkt(G, EXAMINE, ok(pkt(G, LOCATE, 0, name('Made on the Amiga \xe4.txt'), -2), 'Lock'), FIB >> 2), 'Examine')
chk(fib()['size'] == 100000 and fib()['days'] == 17841 and fib()['mins'] == 13 * 60 + 37, 'size and date of a new file: %s' % fib())
h4, r = openf(G, FINDUPDATE, 'Made on the Amiga \xe4.txt')
pkt(G, SEEK, h4, 50000, -1)
write(G, h4, b'X' * 10)
chk(pkt(G, SETSIZE, h4, 60000, -1)[0] == 60000, 'SetFileSize shorter')
ok(pkt(G, END, h4), 'Close')
data = data[:50000] + b'X' * 10 + data[50010:60000]
lk = ok(pkt(G, LOCATE, 0, name('readme.txt'), -1), 'exclusive lock')
err(openf(G, FINDINPUT, 'readme.txt')[1], 202, 'a file with an exclusive lock')
err(pkt(G, LOCATE, 0, name('readme.txt'), -2), 202, 'a second lock')
err(pkt(G, DELETE, 0, name('readme.txt')), 202, 'delete a locked file')
ok(pkt(G, FREE, lk), 'UnLock')
s1 = ok(pkt(G, LOCATE, 0, name('readme.txt'), -2), 'shared')
s2 = ok(pkt(G, LOCATE, 0, name('README.TXT'), -2), 'shared 2')
chk(pkt(G, SAMELOCK, s1, s2)[0] == -1 and pkt(G, SAMELOCK, s1, root)[0] == 0, 'SameLock')
ok(pkt(G, FREE, s1), 'UnLock'); ok(pkt(G, FREE, s2), 'UnLock')

# folders
sv = ok(pkt(G, MKDIR, 0, name('Saves')), 'CreateDir')
ok(pkt(G, FREE, sv), 'UnLock')
err(pkt(G, MKDIR, 0, name('saves')), 203, 'CreateDir of one that is there')
hs, r = openf(G, FINDOUTPUT, 'Saves/slot1')
write(G, hs, b'level 7'); ok(pkt(G, END, hs), 'Close')
sub = ok(pkt(G, LOCATE, 0, name('Saves'), -2), 'Lock Saves')
hs, r = openf(G, FINDOUTPUT, 'slot2', sub)
write(G, hs, b'level 8'); ok(pkt(G, END, hs), 'Close')
par = ok(pkt(G, PARENT, sub), 'ParentDir')
chk(pkt(G, SAMELOCK, par, root)[0] == -1, 'the parent of Saves is the root')
ok(pkt(G, FREE, par), 'UnLock')
ok(pkt(G, RENAME, 0, name('Saves/slot1'), 0, name('slot one, moved')), 'Rename into another folder')
ok(pkt(G, RENAME, 0, name('readme.txt'), 0, name('ReadMe.txt')), 'Rename: only the case')
err(pkt(G, RENAME, 0, name('Saves'), sub, name('inside')), 202, 'a folder into itself')
err(pkt(G, DELETE, 0, name('Saves')), 202, 'delete a locked folder')
ok(pkt(G, FREE, sub), 'UnLock')
err(pkt(G, DELETE, 0, name('Saves')), 216, 'delete a folder that is not empty')
ok(pkt(G, DELETE, 0, name('Saves/slot2')), 'Delete a file')
ok(pkt(G, DELETE, 0, name('Saves')), 'Delete a folder')
ok(pkt(G, PROTECT, 0, 0, name('ReadMe.txt'), 0x05), 'SetProtection: no write, no delete')
err(pkt(G, DELETE, 0, name('ReadMe.txt')), 222, 'delete a delete-protected file')
err(openf(G, FINDOUTPUT, 'ReadMe.txt')[1], 223, 'write a write-protected file')
ls = {e['name']: e for e in listdir(G, root)}
chk(ls['ReadMe.txt']['prot'] & 0x5 == 0x5, 'protection shows: %x' % ls['ReadMe.txt']['prot'])
ok(pkt(G, PROTECT, 0, 0, name('ReadMe.txt'), 0), 'SetProtection back')
ok(pkt(G, DISKINFO, INFO >> 2), 'Info')
nb, used, bpb = mem.r32(INFO + 12), mem.r32(INFO + 16), mem.r32(INFO + 20)
chk(bpb == 512 and 70000 < nb * 512 < 41_000_000 and 0 < used < nb and mem.r32(INFO + 24) == 0x444f5300, 'InfoData %d %d %d' % (nb, used, bpb))
chk(pkt(G, ISFS)[0] == -1 and pkt(G, CURVOL, 0)[0] == dl >> 2, 'IsFileSystem, current volume')
ok(pkt(G, FREE, root), 'UnLock root')

# the hardfiles folder: raw.hdf is mounted (a unit): never written through the folder
HD = start(3)
hh, r = openf(HD, FINDINPUT, 'raw.hdf')
chk(hh != 0 and readall(HD, hh, 65536) == files['HDF/raw.hdf'], 'read a mounted hardfile')
chk(write(HD, hh, b'oops') == (-1, 202), 'write a mounted hardfile: refused')
chk(pkt(HD, SETSIZE, hh, 0, -1) == (-1, 202), 'shorten a mounted hardfile: refused')
ok(pkt(HD, END, hh), 'Close')
err(openf(HD, FINDOUTPUT, 'raw.hdf')[1], 202, 'replace a mounted hardfile')
err(pkt(HD, DELETE, 0, name('raw.hdf')), 202, 'delete a mounted hardfile')
hn, r = openf(HD, FINDOUTPUT, 'new.hdf')
chk(hn and write(HD, hn, b'\0' * 1024)[0] == 1024, 'a new hardfile can be made (mounted at the next boot)')
ok(pkt(HD, END, hn), 'Close')
DO = start(2)
lsd = [e['name'] for e in listdir(DO, ok(pkt(DO, LOCATE, 0, name(''), -2), 'Lock'))]
chk(lsd == ['letter.txt'], 'Documents: %s' % lsd)
ok(pkt(DO, EXAMINE, ok(pkt(DO, LOCATE, 0, name(''), -2), 'Lock'), FIB >> 2), 'Examine')
chk(fib()['name'] == 'Documents', 'the volume name, not the folder name')
print('packets OK: %d checks, %d card reads, %d writes' % (checks[0], stats['reads'], stats['writes']))

# ---- what mtools and fsck.fat say about the card ------------------------------------------------------
open(img, 'wb').write(card)
fs = subprocess.run(['fsck.fat', '-n', img], capture_output=True, text=True)
if fs.returncode != 0 or any(k in fs.stdout.lower() for k in ('differ', 'duplicate', 'lost', 'orphan', 'bad', 'invalid', 'free cluster summary')):
    die('fsck.fat:\n' + fs.stdout + fs.stderr)
def mtype(p):
    return subprocess.run(['mtype', '-i', img, '::/' + p], capture_output=True, env=env, check=True).stdout
chk(mtype('Games/Made on the Amiga ä.txt') == data, 'mtools reads the written file')
chk(mtype('Games/slot one, moved') == b'level 7', 'mtools: the moved file')
chk(mtype('Games/ReadMe.txt') == files['Games/readme.txt'], 'mtools: the renamed file')
chk(mtype('HDF/raw.hdf') == files['HDF/raw.hdf'], 'mtools: the hardfile untouched')
listing = subprocess.run(['mdir', '-b', '-i', img, '::/Games'], capture_output=True, text=True, env=env).stdout
chk('Saves' not in listing, 'Saves deleted: %s' % listing)
print('card: fsck.fat clean, mtools reads it back')
print('ALL OK')
