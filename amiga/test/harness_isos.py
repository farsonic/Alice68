#!/usr/bin/env python3
"""alice68hdf.device's CD images (amiga/hdf/isos.c, iso9660.c) on a 68k emulator
(machine68k / Musashi): a FAT32 card made by mkfs.fat and mtools with ISO
images made by genisoimage (Rock Ridge + Joliet, Joliet only, a second image
with the same label, a broken one, a macOS "._" file) and a folder over them;
a fake exec, expansion.library, dos.library and brcm-emmc.device.

    harness_isos.py DEVICE.hunk DEVICE.elf WORKDIR [isooff]

Checks the boot nodes the device makes for the images, then starts a handler
(iso_start) and sends it DOS packets (iso_do): listing, reading, seeking,
paths, parents, locks, info, and every kind of write refused. A mounted image
must be read-only through the folder too.
SPDX-License-Identifier: GPL-2.0-only"""
import os, random, struct, subprocess, sys, zlib
import machine68k as M
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'folder'))
import mkinfo, mkdiskicons
ICON = mkdiskicons.disk_icon(mkinfo.icon_cdrom)

R = M.Register
mach = M.Machine(M.CPUType.M68020, 16384)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYSBASE, EXPBASE, EMMC, DOSBASE = 0x8000, 0xA000, 0xC000, 0xE000
LOAD, DEVBASE, CONFIGDEV, PORT = 0x20000, 0x70000, 0x7F000, 0x7F800
HEAP, HEAP_END = 0x400000, 0xC00000
STACK = 0x200000
hunk, elf, work = sys.argv[1:4]
iso_off = 'isooff' in sys.argv[4:]
card_ro = False
env = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8')

def die(msg):
    print('FAIL:', msg); sys.exit(1)

# ---- the images and the card -----------------------------------------------------------
img = os.path.join(work, 'isos.img')
src = os.path.join(work, 'isos-src')
subprocess.run(['rm', '-rf', src, img], check=True)
os.makedirs(os.path.join(src, 'card', 'ISO'))
rnd = random.Random(1992)
def blob(n): return bytes(rnd.getrandbits(8) for _ in range(n))
discs = {
    'rr': {'Read Me.txt': b'Alice68 CD\n', 'Big File.bin': blob(150000), 'Docs/sub dir/file.txt': b'nested\n',
           'a/b/c/d/e/f/g/h/i/deep.txt': b'nine folders down\n', 'empty': b''},
    'joliet': {'Game.exe': blob(5000), 'Data/level1.dat': blob(3000)},
    'dup': {'other.txt': b'same label\n'},
}
def make_iso(kind, label, path, opts):
    d = os.path.join(src, 'tree-' + kind)
    for p, b in discs[kind].items():
        os.makedirs(os.path.dirname(os.path.join(d, p)) or d, exist_ok=True)
        open(os.path.join(d, p), 'wb').write(b)
    subprocess.run(['genisoimage', '-quiet', '-input-charset', 'utf-8', '-V', label, '-o', path] + opts + [d],
                   check=True, stderr=subprocess.DEVNULL)
make_iso('rr', 'Alice68 CD', os.path.join(src, 'card', 'ISO', 'Test Disc.iso'), ['-R', '-J'])
make_iso('joliet', 'Game Disc', os.path.join(src, 'card', 'Game.iso'), ['-J'])
make_iso('dup', 'Game Disc', os.path.join(src, 'card', 'ISO', 'dup.iso'), ['-J'])
open(os.path.join(src, 'card', 'ISO', 'broken.iso'), 'wb').write(blob(65536))
open(os.path.join(src, 'card', 'ISO', '._Test Disc.iso'), 'wb').write(b'\0' * 4096)
open(os.path.join(src, 'card', 'config.txt'), 'w').write('[all]\nalice68.folder=ISO:Images\n')
if iso_off: open(os.path.join(src, 'card', 'cmdline.txt'), 'w').write('iso=off\n')
subprocess.run(['mkfs.fat', '-F', '32', '-s', '1', '-C', img, '40000'], check=True, stdout=subprocess.DEVNULL)
for top in sorted(os.listdir(os.path.join(src, 'card'))):
    subprocess.run(['mcopy', '-s', '-i', img, os.path.join(src, 'card', top), '::/'], check=True, env=env)
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
cds = [b for b in boots if b['dev'] == 'alice68hdf.device' and b['unit'] >= 0x5000]
if iso_off:
    if cds or 'CD images turned off (iso=off)' not in logtxt: die('iso=off: %d CD boot nodes' % len(cds))
    print('iso=off: no CD images mounted'); print('ALL OK'); sys.exit(0)
got = sorted((b['name'], b['unit'] - 0x5000) for b in cds)
if got != [('CD0', 0), ('CD1', 1), ('CD2', 2)]: die('CD boot nodes %s' % got)
for b in cds:
    dn = b['dn']
    seg, gv, stack = mem.r32(dn + 32), mem.r32(dn + 36), mem.r32(dn + 20)
    if b['flags'] != 1 or b['pri'] != -128 or gv != 0xffffffff or stack < 4096 or b['env'][16] != 0x43443031 or b['env'][1] != 512:
        die('%s: flags %d pri %d globvec %x stack %d env %s' % (b['name'], b['flags'], b['pri'], gv, stack, b['env']))
    c = (seg << 2) + 4
    if (mem.r16(c), mem.r32(c + 2), mem.r16(c + 6), mem.r32(c + 8)) != (0x4879, DEVBASE, 0x4eb9, syms['iso_main']):
        die('the seglist does not call iso_main(db)')
vols = {}
import re
for m in re.finditer(r'CD image (.+?): volume (.+?) \((CD\d):\), ([A-Za-z0-9 ]+?), \d+ MB, read-only', logtxt):
    vols[m.group(1)] = (m.group(2), m.group(3), m.group(4))
if set(vols) != {'Game.iso', 'Test Disc.iso', 'dup.iso'}: die('CD images mounted: %s' % vols)
if vols['Test Disc.iso'][0] != 'Alice68 CD' or vols['Test Disc.iso'][2] != 'Rock Ridge': die('Test Disc: %s' % (vols['Test Disc.iso'],))
if vols['Game.iso'][2] != 'Joliet' or {vols['Game.iso'][0], vols['dup.iso'][0]} != {'Game Disc', 'Game Disc2'}:
    die('the same label twice: %s %s' % (vols['Game.iso'], vols['dup.iso']))
if 'broken.iso: not an ISO 9660 CD image - ignored' not in logtxt: die('the broken image')
if '._Test' in logtxt: die('a macOS dot file was taken')
if 'hardfiles: 0 bootable, 0 non-bootable, 0 left out' not in logtxt: die('the status line changed')
print('boot nodes: %s; volumes %s' % (', '.join(g[0] for g in got), ', '.join('%s=%s' % (v[1], v[0]) for v in vols.values())))

# ---- a handler and its packets ---------------------------------------------------------------------
unit = int(vols['Test Disc.iso'][1][2])
X = call(syms['iso_start'], args=(DEVBASE, 0x5000 + unit, PORT), name='iso_start')
if not X: die('iso_start failed')
dl = doslist[-1]
if mem.r32(dl + 4) != 2 or mem.r32(dl + 8) != PORT or rbstr(mem.r32(dl + 40)) != 'Alice68 CD' or mem.r32(dl + 32) != 0x444f5300:
    die('volume node: type %d task %x name %s' % (mem.r32(dl + 4), mem.r32(dl + 8), rbstr(mem.r32(dl + 40))))
if mem.r32(dl + 16) < 17000: die('volume date %d' % mem.r32(dl + 16))
PKT = alloc(48)
BUF = alloc(0x40000)
FIB = (alloc(264) + 3) & ~3
INFO = (alloc(40) + 3) & ~3
names = {}
def name(s):
    if s not in names: names[s] = bstr(s)
    return names[s]
def pkt(typ, *args, ctx=None, fn='iso_do'):
    mem.w_block(PKT, bytes(48))
    mem.w32(PKT + 8, typ)
    for i, a in enumerate(args): mem.w32(PKT + 20 + 4 * i, a & 0xffffffff)
    call(syms[fn], args=(ctx or X, PKT), name='packet %d' % typ)
    r1, r2 = mem.r32(PKT + 12), mem.r32(PKT + 16)
    return (r1 - (1 << 32) if r1 & 0x80000000 else r1), r2
checks = [0]
def chk(c, what):
    if not c: die(what)
    checks[0] += 1
def ok(res, what):
    if res[0] == 0: die('%s: failed, error %d' % (what, res[1]))
    return res[0]
def err(res, code, what):
    if res[0] not in (0, -1) or res[1] != code: die('%s: got %s, want error %d' % (what, res, code))
    checks[0] += 1
LOCATE, FREE, DELETE, RENAME, COPYDIR, PROTECT, MKDIR, EXAMINE, EXNEXT, DISKINFO, INFOP, PARENT, SAMELOCK = 8, 15, 16, 17, 19, 21, 22, 23, 24, 25, 26, 29, 40
FINDUPDATE, FINDINPUT, FINDOUTPUT, END, SEEK, SETSIZE, ISFS, CURVOL = 1004, 1005, 1006, 1007, 1008, 1022, 1027, 7
def fib():
    t = mem.r32(FIB + 4)
    return dict(key=mem.r32(FIB), type=t - (1 << 32 if t & 0x80000000 else 0),
                name=bytes(mem.r_block(FIB + 9, mem.r8(FIB + 8))).decode('latin1'), prot=mem.r32(FIB + 116),
                size=mem.r32(FIB + 124), days=mem.r32(FIB + 132))
def listdir(lock):
    ok(pkt(EXAMINE, lock, FIB >> 2), 'Examine')
    out = []
    while True:
        r = pkt(EXNEXT, lock, FIB >> 2)
        if r[0] == 0:
            if r[1] != 232: die('ExNext error %d' % r[1])
            return out
        out.append(fib())
def openf(typ, path, lock=0):
    f = (alloc(48) + 3) & ~3
    r = pkt(typ, f >> 2, lock, name(path))
    return (mem.r32(f + 36) if r[0] else 0), r
def readall(h, chunk):
    out = b''
    while True:
        n = pkt(ord('R'), h, BUF + 1, chunk)[0]                 # an odd buffer address
        if n < 0: die('Read error')
        if n == 0: return out
        out += bytes(mem.r_block(BUF + 1, n))

root = ok(pkt(LOCATE, 0, name(''), -2), 'Lock root')
ok(pkt(EXAMINE, root, FIB >> 2), 'Examine root')
chk(fib()['type'] == 1 and fib()['name'] == 'Alice68 CD', 'root: %s' % fib())
chk(pkt(PARENT, root) == (0, 0), 'the root has no parent')
ls = {e['name']: e for e in listdir(root)}
chk(set(ls) == {'Read Me.txt', 'Big File.bin', 'Docs', 'a', 'empty', 'Disk.info'}, 'root lists %s' % sorted(ls))
chk(ls['Disk.info']['size'] == len(ICON) and ls['Disk.info']['type'] == -3, 'the built-in Disk.info: %s' % ls['Disk.info'])
hi, r = openf(FINDINPUT, 'Disk.info')
chk(hi and readall(hi, 100) == ICON and mkinfo.read_icon(ICON)[0] == mkinfo.WBDISK, 'the built-in Disk.info reads as a disk icon')
chk(mkinfo.drawer_show(ICON) == (mkinfo.DDFLAGS_SHOWALL, mkinfo.DDVM_BYICON), 'the CD\'s window shows all files')
ok(pkt(END, hi), 'Close')
err(openf(FINDOUTPUT, 'Disk.info')[1], 214, 'write Disk.info on a CD image')
dl_ = ok(pkt(LOCATE, 0, name('Disk.info'), -2), 'Lock Disk.info')
pd_ = ok(pkt(PARENT, dl_), 'its parent')
chk(pkt(SAMELOCK, pd_, root)[0] == -1, 'the built-in Disk.info is in the root')
ok(pkt(FREE, dl_), 'UnLock'); ok(pkt(FREE, pd_), 'UnLock')
dsub = ok(pkt(LOCATE, 0, name('Docs'), -2), 'Lock Docs')
chk('Disk.info' not in [e['name'] for e in listdir(dsub)], 'no Disk.info in sub-folders')
err(pkt(LOCATE, dsub, name('Disk.info'), -2), 205, 'Disk.info in a sub-folder')
ok(pkt(FREE, dsub), 'UnLock')
chk(ls['Docs']['type'] == 2 and ls['Big File.bin']['type'] == -3 and ls['Big File.bin']['size'] == 150000, 'types and sizes')
chk(ls['Read Me.txt']['prot'] & 0x5 == 0x5 and ls['Read Me.txt']['days'] > 17000, 'read-only, dated: %s' % ls['Read Me.txt'])
h1, r = openf(FINDINPUT, 'Big File.bin')
chk(h1 and readall(h1, 4093) == discs['rr']['Big File.bin'], 'read a file in odd pieces into an odd address')
chk(pkt(SEEK, h1, 100000, -1)[0] == 150000, 'Seek returns the old position')
n_ = pkt(ord('R'), h1, BUF, 10)[0]
chk(n_ == 10 and bytes(mem.r_block(BUF, 10)) == discs['rr']['Big File.bin'][100000:100010], 'read after Seek')
err(pkt(SEEK, h1, 150001, -1), 219, 'Seek past the end')
err(pkt(ord('W'), h1, BUF, 10), 214, 'Write')
err(pkt(SETSIZE, h1, 0, -1), 214, 'SetFileSize')
ok(pkt(END, h1), 'Close')
h2, r = openf(FINDINPUT, 'Alice68 CD:docs/SUB DIR/File.TXT')
chk(h2 and readall(h2, 512) == b'nested\n', 'volume prefix, any case, nested folders')
ok(pkt(END, h2), 'Close')
h3, r = openf(FINDINPUT, 'a/b/c/d/e/f/g/h/i/deep.txt')
chk(h3 and readall(h3, 100) == b'nine folders down\n', 'a folder Rock Ridge relocated (deeper than 8)')
ok(pkt(END, h3), 'Close')
h4, r = openf(FINDINPUT, 'empty')
chk(h4 and readall(h4, 100) == b'', 'an empty file')
ok(pkt(END, h4), 'Close')
err(openf(FINDINPUT, 'nothing.here')[1], 205, 'a file that is not there')
err(openf(FINDINPUT, 'Docs')[1], 212, 'a folder as a file')
err(openf(FINDINPUT, 'Nope/x')[1], 204, 'a path through a missing folder')
err(openf(FINDOUTPUT, 'new.txt')[1], 214, 'MODE_NEWFILE')
err(openf(FINDUPDATE, 'Read Me.txt')[1], 214, 'MODE_READWRITE')
err(pkt(DELETE, 0, name('Read Me.txt')), 214, 'Delete')
err(pkt(RENAME, 0, name('Read Me.txt'), 0, name('x')), 214, 'Rename')
err(pkt(MKDIR, 0, name('New')), 214, 'CreateDir')
err(pkt(PROTECT, 0, 0, name('Read Me.txt'), 0), 214, 'SetProtection')
sub = ok(pkt(LOCATE, 0, name('Docs/sub dir'), -2), 'Lock a sub-folder')
par = ok(pkt(PARENT, sub), 'ParentDir')
docs = ok(pkt(LOCATE, 0, name('Docs'), -2), 'Lock Docs')
chk(pkt(SAMELOCK, par, docs)[0] == -1 and pkt(SAMELOCK, par, root)[0] == 0, 'the parent of Docs/sub dir is Docs')
gp = ok(pkt(PARENT, docs), 'ParentDir of Docs')
chk(pkt(SAMELOCK, gp, root)[0] == -1, 'the parent of Docs is the root')
deep = ok(pkt(LOCATE, 0, name('a/b/c/d/e/f/g/h/i'), -2), 'Lock the relocated folder')
dp = ok(pkt(PARENT, deep), 'its parent')
h8 = ok(pkt(LOCATE, 0, name('a/b/c/d/e/f/g/h'), -2), 'Lock a/.../h')
chk(pkt(SAMELOCK, dp, h8)[0] == -1, 'the relocated folder\'s parent is where it was (Rock Ridge PL)')
up = ok(pkt(LOCATE, sub, name('//Read Me.txt'), -2), 'a path with "/" from a lock')
ok(pkt(EXAMINE, up, FIB >> 2), 'Examine')
chk(fib()['name'] == 'Read Me.txt', 'up two folders: %s' % fib())
for l in (sub, par, docs, gp, deep, dp, h8, up): ok(pkt(FREE, l), 'UnLock')
ok(pkt(DISKINFO, INFO >> 2), 'Info')
chk(mem.r32(INFO + 8) == 80 and mem.r32(INFO + 20) == 2048 and mem.r32(INFO + 12) > 70 and mem.r32(INFO + 24) == 0x444f5300,
    'InfoData: state %d bytes %d blocks %d' % (mem.r32(INFO + 8), mem.r32(INFO + 20), mem.r32(INFO + 12)))
chk(pkt(ISFS)[0] == -1 and pkt(CURVOL, 0)[0] == dl >> 2, 'IsFileSystem, current volume')
ok(pkt(FREE, root), 'UnLock root')
print('packets OK: %d checks, %d card reads' % (checks[0], stats['reads']))

# ---- the folder over the images: a mounted one is never written ------------------------------------------
fold = [b for b in boots if b['dev'] == 'alice68hdf.device' and 0x4000 <= b['unit'] < 0x5000]
if len(fold) != 1: die('the Images folder: %d boot nodes' % len(fold))
F = call(syms['fold_start'], args=(DEVBASE, fold[0]['unit'], PORT), name='fold_start')
if not F: die('fold_start failed')
before = bytes(card)
err(openf(FINDOUTPUT, 'Test Disc.iso')[1] if False else pkt(FINDOUTPUT, (alloc(48) + 3) >> 2, 0, name('Test Disc.iso'), ctx=F, fn='fold_do'), 202, 'replace a mounted image through the folder')
err(pkt(DELETE, 0, name('dup.iso'), ctx=F, fn='fold_do'), 202, 'delete a mounted image through the folder')
ok(pkt(DELETE, 0, name('broken.iso'), ctx=F, fn='fold_do'), 'an image that is not mounted can be deleted')
print('folder: mounted images refused (202), others writable')
print('ALL OK')
