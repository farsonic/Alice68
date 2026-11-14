#!/usr/bin/env python3
"""alice68hdf.device's folders (amiga/hdf/folders.c) on a 68k emulator
(machine68k / Musashi): a FAT32 card made by mkfs.fat and mtools with a
config.txt, the ALICE68 folder, other folders and a mounted hardfile; a fake
exec, expansion.library, dos.library and brcm-emmc.device.

    harness_folders.py DEVICE.hunk DEVICE.elf WORKDIR [cardro] [exfat | exfatrw | exfatboot [hdfboot]]

exfat: the folders (all but ALICE68) on an exFAT partition after the FAT32 one
(mkfs.exfat, filled through exfat-fuse; needs root and losetup): mounted
read-only, read through the same packets, every write refused; the partition
itself is a volume too. exfatrw: the same with alice68.exfat=rw: files made,
written, renamed, deleted, a comment kept in a .uaem sidecar, on a folder and on
the partition's own volume; then fsck.exfat and exfat-fuse read the card back.
exfatboot: alice68.exfat=boot: the partition's volume is the boot volume (a boot
node ahead of every other drive), files in S are scripts unless a sidecar says
otherwise; with hdfboot, hdf.boot= in cmdline.txt wins instead.

Checks the boot nodes the device makes for the folders, then starts a
handler (fold_start) and sends it DOS packets (fold_do): directory listing,
reading, writing, locks, renaming, deleting, the mounted hardfile kept
read-only. At the end mtools must read back what the packets wrote and
fsck.fat must find nothing wrong with the card.
SPDX-License-Identifier: GPL-2.0-only"""
import os, random, struct, subprocess, sys
import machine68k as M
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'folder'))
import mkinfo, mkdiskicons
ICON = mkdiskicons.disk_icon(mkinfo.icon_sdfolder)

R = M.Register
mach = M.Machine(M.CPUType.M68020, 16384)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYSBASE, EXPBASE, EMMC, DOSBASE = 0x8000, 0xA000, 0xC000, 0xE000
LOAD, DEVBASE, CONFIGDEV, PORT = 0x20000, 0x70000, 0x7F000, 0x7F800
HEAP, HEAP_END = 0x400000, 0xC00000
STACK = 0x200000
hunk, elf, work = sys.argv[1:4]
card_ro = 'cardro' in sys.argv[4:]
exfatboot = 'exfatboot' in sys.argv[4:]
hdfboot = 'hdfboot' in sys.argv[4:]
exfatrw = 'exfatrw' in sys.argv[4:] or exfatboot
exfat = 'exfat' in sys.argv[4:] or exfatrw
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
    'ALICE68/Disk.info': b'its own icon',
    'Games/Long File Name.txt': blob(70000),
    'Games/readme.txt': b'hello from the PC\n',
    'Games/Sub/deep.bin': blob(3000),
    'Work/Docs/letter.txt': b'Dear Amiga,\n',
    # made on a PC or in FS-UAE: a sidecar with the script bit and a comment, an escaped name, clutter
    'Games/Sub/Script': b'echo hi\n',
    'Games/Sub/Script.uaem': b'-s--rwed 2020-05-06 07:08:09.10 made on a PC\n',
    'Games/Sub/What%3f.txt': b'question\n',
    'Games/Sub/._deep.bin': bytes(32),
    'Games/Sub/.DS_Store': b'x',
    # a Workbench's S folder copied from a PC: no sidecars
    'Work/S/Startup-Sequence': b'echo "booted from exFAT"\n',
    'Work/S/User-Startup': b'; nothing\n',
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
          '  alice68.folder=ALICE68  \n' + ('alice68.exfat=boot\n' if exfatboot else 'alice68.exfat=rw\n' if exfatrw else ''))
open(os.path.join(src, 'config.txt'), 'w').write(config)
if not exfat:
    subprocess.run(['mkfs.fat', '-F', '32', '-s', '1', '-C', img, '40000'], check=True, stdout=subprocess.DEVNULL)
    for top in ('ALICE68', 'Games', 'Work', 'HDF', 'config.txt'):
        subprocess.run(['mcopy', '-s', '-i', img, os.path.join(src, top), '::/'], check=True, env=env)
else:
    # MBR: FAT32 at 2048 (ALICE68, config.txt), exFAT after it with Games, Work and HDF
    P1, N1, P2, N2 = 2048, 69632, 71680, 65536
    with open(img, 'wb') as f: f.truncate((P2 + N2) * 512)
    subprocess.run(['sfdisk', '-q', img], input=b'label: dos\n%d,%d,c\n%d,%d,7\n' % (P1, N1, P2, N2), check=True)
    def loopdev(st, n):
        return subprocess.run(['losetup', '-f', '--show', '-o', str(st * 512), '--sizelimit', str(n * 512), img],
                              check=True, capture_output=True).stdout.decode().strip()
    l1 = loopdev(P1, N1)
    subprocess.run(['mkfs.fat', '-F', '32', '-s', '1', l1], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if hdfboot: open(os.path.join(src, 'cmdline.txt'), 'w').write('hdf.boot=raw\n')
    for top in ('ALICE68', 'config.txt') + (('cmdline.txt',) if hdfboot else ()):
        subprocess.run(['mcopy', '-s', '-i', l1, os.path.join(src, top), '::/'], check=True, env=env, stderr=subprocess.DEVNULL)
    subprocess.run(['losetup', '-d', l1], check=True)
    l2 = loopdev(P2, N2)
    subprocess.run(['mkfs.exfat', l2], check=True, stdout=subprocess.DEVNULL)
    mnt = os.path.join(work, 'folders-mnt'); os.makedirs(mnt, exist_ok=True)
    subprocess.run(['mount.exfat-fuse', l2, mnt], check=True, capture_output=True)
    try:
        subprocess.run(['cp', '-r'] + [os.path.join(src, t) for t in ('Games', 'Work', 'HDF')] + [mnt + '/'], check=True)
    finally:
        subprocess.run(['umount', mnt], check=True); subprocess.run(['losetup', '-d', l2], check=True)
    if exfatrw:                                      # as after a power cut: the boot's validator must run first
        with open(img, 'r+b') as f:
            f.seek(P2 * 512 + 106); fl = int.from_bytes(f.read(2), 'little'); f.seek(P2 * 512 + 106); f.write((fl | 2).to_bytes(2, 'little'))
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
# Alice68's USB card (pt_hdr), for Ctrl-Amiga-Amiga: alice68hdf.device's vertical blank server
PTB = alloc(256); mem.w32(PTB, 0x45415550); mem.w32(PTB + 60, 1)    # reset_req left over from before: cleared at Init
PTCD = alloc(48); mem.w32(PTCD + 32, PTB); mem.w32(PTCD + 36, 0x10000)
lvo(EXPBASE, -72, lambda o, p: setd0((PTCD if d(R.D1) == 0x6a else CONFIGDEV) if d(R.D0) == 0x6d73 else 0))
intservers = []
lvo(SYSBASE, -168, lambda o, p: intservers.append((d(R.D0), d(R.A1))))
coldreboot = []
lvo(SYSBASE, -726, lambda o, p: coldreboot.append(mem.r32(PTB + 60)))
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
# Ctrl-Amiga-Amiga: the server is there, does nothing unasked, and takes the kernel's request with ColdReboot
vb = [a for n, a in intservers if n == 5]
if len(vb) != 1 or mem.r32(vb[0] + 14) != DEVBASE or mem.r32(vb[0] + 18) != syms['stub_vbl'] or mem.r8(vb[0] + 8) != 2:
    die('the vertical blank server: %s' % intservers)
if mem.r32(PTB + 60) != 0: die('a request left over from before Init was not cleared')
call(syms['stub_vbl'], {R.A1: DEVBASE}, name='vbl (idle)')
if coldreboot: die('ColdReboot without a request')
mem.w32(PTB + 60, 1)
call(syms['stub_vbl'], {R.A1: DEVBASE}, name='vbl (Ctrl-Amiga-Amiga)')
if coldreboot != [2] or mem.r32(PTB + 60) != 2: die('Ctrl-Amiga-Amiga: ColdReboot %s, reset_req %d' % (coldreboot, mem.r32(PTB + 60)))
print('Ctrl-Amiga-Amiga: vertical blank server installed; idle does nothing; a request is taken (2) and ColdReboot called')

# ---- the boot nodes -----------------------------------------------------------------------------
fold = [b for b in boots if b['dev'] == 'alice68hdf.device' and b['unit'] >= 0x4000]
got = [(b['name'], b['unit'] - 0x4000, b['pri']) for b in fold]
want = [('SDF0', 0, -128), ('SDF1', 1, -128), ('SDF2', 2, -128), ('SDF3', 3, -128)] + ([('SDF4', 4, 0 if exfatboot and not hdfboot else -128)] if exfat else [])
if got != want: die('folder boot nodes %s, want %s' % (got, want))
for b in fold:
    dn = b['dn']
    seg, gv, stack = mem.r32(dn + 32), mem.r32(dn + 36), mem.r32(dn + 20)
    if b['flags'] != 1 or gv != 0xffffffff or stack < 8192 or b['env'][16] != 0x46415401 or b['env'][15] != (b['pri'] & 0xffffffff):
        die('%s: flags %d globvec %x stack %d env %s' % (b['name'], b['flags'], gv, stack, b['env']))
    c = (seg << 2) + 4
    if (mem.r16(c), mem.r32(c + 2), mem.r16(c + 6), mem.r32(c + 8), mem.r16(c + 12), mem.r16(c + 14)) != \
       (0x4879, DEVBASE, 0x4eb9, syms['folder_main'], 0x588f, 0x4e75):
        die('the seglist does not call folder_main(db)')
X = ' (exFAT)' if exfat else ''
for line in ('folder ALICE68: volume ALICE68 (SDF0:)', 'folder Games%s: volume Games (SDF1:)' % X,
             'folder Work/Docs%s: volume Documents (SDF2:)' % X, 'folder HDF%s: volume Hardfiles (SDF3:)' % X,
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

if exfat:
    want_line = 'exFAT partition: volume exFAT (SDF4:), ' + ('bootable' if exfatboot and not hdfboot else 'not bootable') + ('' if exfatrw else ', read-only: alice68.exfat=rw in config.txt writes it')
    if exfatboot: chk(('hdf.boot=raw boots instead' in logtxt) == hdfboot and ('boots as exFAT (priority 0)' in logtxt) != hdfboot, 'the boot choice in the log')
    chk(want_line in logtxt, 'log: %s' % want_line)
    X4 = start(4)
    chk(sorted(e['name'] for e in listdir(X4, ok(pkt(X4, LOCATE, 0, name(''), -2), 'Lock'))) == ['Disk.info', 'Games', 'HDF', 'Work'], 'the partition volume lists its root')
    if hdfboot: print('hdf.boot= wins over alice68.exfat=boot'); print('ALL OK'); sys.exit(0)
if exfatboot:                                        # a Workbench's S folder from a PC: scripts
    def prot(ctx, path):
        l = ok(pkt(ctx, LOCATE, 0, name(path), -2), 'Lock ' + path); ok(pkt(ctx, EXAMINE, l, FIB >> 2), 'Examine'); p_ = fib()['prot']; ok(pkt(ctx, FREE, l), 'UnLock'); return p_
    chk(prot(X4, 'Work/S/Startup-Sequence') & 0x4f == 0x40 and prot(X4, 'Work/S/User-Startup') & 0x40, 'files in S are scripts')
    chk(not prot(X4, 'Work/Docs/letter.txt') & 0x40 and not prot(X4, 'Work/S') & 0x40, 'other files and the folder itself are not')
    sl = ok(pkt(X4, LOCATE, 0, name('Work/S'), -2), 'Lock S')
    chk(all(e['prot'] & 0x40 for e in listdir(X4, sl)), 'a listing of S shows the script bit too')
    ok(pkt(X4, PROTECT, 0, 0, name('Work/S/User-Startup'), 0), 'SetProtection ----rwed on a file in S')
    chk(not prot(X4, 'Work/S/User-Startup') & 0x40, 'the script bit cleared on purpose stays cleared (a sidecar)')
    ok(pkt(X4, PROTECT, 0, 0, name('Work/S/User-Startup'), 0x40), 'SetProtection s back')
    chk(prot(X4, 'Work/S/User-Startup') & 0x40, 'and set again')
if exfatrw:                                          # writing exFAT through the packets
    chk('exFAT: not cleanly removed: checked' in logtxt and 'not written' not in logtxt, 'the boot validator ran and allowed writing')
    ok(pkt(G, MKDIR, 0, name('Made here')), 'CreateDir on exFAT')
    data = bytes(range(256)) * 300
    h, r = openf(G, FINDOUTPUT, 'Made here/file \xe4.bin'); chk(h and write(G, h, data)[0] == len(data), 'write 76800 bytes'); ok(pkt(G, END, h), 'Close')
    h, r = openf(G, FINDINPUT, 'Made here/FILE \xc4.BIN'); chk(h and readall(G, h, 1000) == data, 'read it back, another case'); ok(pkt(G, END, h), 'Close')
    h, r = openf(G, FINDUPDATE, 'readme.txt'); write(G, h, b'HELLO'); ok(pkt(G, END, h), 'Close')
    ok(pkt(G, RENAME, 0, name('Long File Name.txt'), 0, name('Made here/Moved, with a longer name than it had.txt')), 'Rename across folders')
    ok(pkt(G, 28, 0, 0, name('readme.txt'), bstr('a comment on exFAT')), 'SetComment (a .uaem sidecar)')
    ok(pkt(G, DELETE, 0, name('Sub/deep.bin')), 'Delete')
    h, r = openf(X4, FINDOUTPUT, 'top level.txt'); write(X4, h, b'on the partition volume'); ok(pkt(X4, END, h), 'Close')
    H3 = start(3)
    err(openf(H3, FINDOUTPUT, 'raw.hdf')[1], 202, 'replace the mounted hardfile through a folder')
    err(pkt(H3, DELETE, 0, name('raw.hdf')), 202, 'delete the mounted hardfile through a folder')
    err(pkt(X4, DELETE, 0, name('HDF/raw.hdf')), 202, 'delete it through the partition volume')
    f_ = fib(); ok(pkt(G, EXAMINE, ok(pkt(G, LOCATE, 0, name('readme.txt'), -2), 'Lock'), FIB >> 2), 'Examine')
    chk(bytes(mem.r_block(FIB + 145, mem.r8(FIB + 144))) == b'a comment on exFAT', 'the comment reads back')
    open(img, 'wb').write(card)
    L = subprocess.run(['losetup', '-f', '--show', '-o', str(71680 * 512), img], capture_output=True, text=True, check=True).stdout.strip()
    try:
        fsck = subprocess.run(['fsck.exfat', '-n', L], capture_output=True, text=True)
        chk(fsck.returncode == 0, 'fsck.exfat:\n' + fsck.stdout + fsck.stderr)
        mnt = os.path.join(work, 'folders-mnt')
        subprocess.run(['mount.exfat-fuse', '-o', 'ro', L, mnt], check=True, capture_output=True)
        try:
            rdf = lambda p: open(os.path.join(mnt, p), 'rb').read()
            chk(rdf('Games/Made here/file \xe4.bin'.encode('latin-1').decode('latin-1')) == data, 'exfat-fuse: the written file')
            chk(rdf('Games/readme.txt') == b'HELLO' + files['Games/readme.txt'][5:], 'exfat-fuse: the update')
            chk(rdf('Games/Made here/Moved, with a longer name than it had.txt') == files['Games/Long File Name.txt'], 'exfat-fuse: the moved file')
            chk(rdf('Games/readme.txt.uaem').endswith(b' a comment on exFAT\n'), 'exfat-fuse: the sidecar')
            chk(not os.path.exists(os.path.join(mnt, 'Games/Sub/deep.bin')), 'exfat-fuse: the deleted file is gone')
            chk(rdf('top level.txt') == b'on the partition volume', 'exfat-fuse: written on the partition volume')
        finally:
            subprocess.run(['umount', mnt])
    finally:
        subprocess.run(['losetup', '-d', L])
    print('exFAT read-write%s: %d checks; fsck.exfat clean; exfat-fuse reads it all back' % (', the boot volume, S scripts' if exfatboot else '', checks[0])); print('ALL OK'); sys.exit(0)
if exfat:                                            # read-only: everything reads, nothing writes
    root = ok(pkt(G, LOCATE, 0, name(''), -2), 'Lock root')
    ok(pkt(G, EXAMINE, root, FIB >> 2), 'Examine root')
    chk(fib()['name'] == 'Games' and fib()['type'] == 1, 'root: %s' % fib())
    ls = {e['name']: e for e in listdir(G, root)}
    chk(set(ls) == {'Long File Name.txt', 'readme.txt', 'Sub', 'Disk.info'}, 'Games lists %s' % sorted(ls))
    chk(ls['Long File Name.txt']['size'] == 70000 and ls['Sub']['type'] == 2 and ls['readme.txt']['type'] == -3, 'sizes, types: %s' % ls)
    for path, chunk in (('readme.txt', 5), ('LONG FILE NAME.TXT', 777), ('Sub/deep.bin', 4096), ('/Games/Sub/../readme.txt', 100)):
        want = files['Games/' + ('readme.txt' if 'readme' in path else 'Long File Name.txt' if 'LONG' in path else 'Sub/deep.bin')]
        if '..' in path: path = 'Sub//readme.txt'                       # AmigaDOS: "/" is the parent
        h, r = openf(G, FINDINPUT, path)
        chk(h and readall(G, h, chunk) == want, 'read %s in %d-byte pieces' % (path, chunk))
        ok(pkt(G, END, h), 'Close')
    sub = ok(pkt(G, LOCATE, 0, name('Sub'), -2), 'Lock Sub')
    chk(sorted(e['name'] for e in listdir(G, sub)) == ['Script', 'What?.txt', 'deep.bin'], 'Sub lists: sidecars and clutter hidden')
    par = ok(pkt(G, PARENT, sub), 'Parent of Sub')
    chk(pkt(G, SAMELOCK, par, root)[0] == -1, 'the parent of Sub is the root')
    deep = ok(pkt(G, LOCATE, 0, name('Sub/deep.bin'), -2), 'Lock deep.bin')
    chk(pkt(G, SAMELOCK, ok(pkt(G, PARENT, deep), 'Parent of deep.bin'), sub)[0] == -1, 'the parent of deep.bin is Sub')
    ok(pkt(G, INFOP, root, INFO >> 2), 'Info')
    chk(mem.r32(INFO + 8) == 80 and mem.r32(INFO + 12) > 0 and mem.r32(INFO + 16) <= mem.r32(INFO + 12), 'InfoData: write-protected, blocks')
    f_ = fh()
    err(pkt(G, FINDOUTPUT, f_ >> 2, 0, name('new.txt')), 214, 'create a file')
    err(pkt(G, FINDOUTPUT, f_ >> 2, 0, name('readme.txt')), 214, 'replace a file')
    err(pkt(G, DELETE, 0, name('readme.txt')), 214, 'delete')
    err(pkt(G, MKDIR, 0, name('NewDir')), 214, 'make a directory')
    err(pkt(G, RENAME, 0, name('readme.txt'), 0, name('x.txt')), 214, 'rename')
    h, r = openf(G, FINDUPDATE, 'readme.txt')
    chk(h, 'open for update (reading still works)')
    err(write(G, h, b'x'), 214, 'write')
    ok(pkt(G, END, h), 'Close')
    DO = start(2)
    chk([e['name'] for e in listdir(DO, ok(pkt(DO, LOCATE, 0, name(''), -2), 'Lock'))] == ['letter.txt', 'Disk.info'], 'Documents lists')
    chk(bytes(card) == open(img, 'rb').read(), 'the card is unchanged')
    print('exFAT folders: read-only, %d checks; writes refused (214); card unchanged' % checks[0]); print('ALL OK'); sys.exit(0)

root = ok(pkt(G, LOCATE, 0, name(''), -2), 'Lock root')
ok(pkt(G, EXAMINE, root, FIB >> 2), 'Examine root')
f0 = fib()
chk(f0['type'] == 1 and f0['name'] == 'Games', 'root: %s' % f0)
chk(pkt(G, PARENT, root)[0] == 0 and pkt(G, PARENT, root)[1] == 0, 'the root has no parent')
ls = {e['name']: e for e in listdir(G, root)}
chk(set(ls) == {'Long File Name.txt', 'readme.txt', 'Sub', 'Disk.info'}, 'Games lists %s' % sorted(ls))
chk(ls['Disk.info']['type'] == -3 and ls['Disk.info']['size'] == len(ICON) and ls['Disk.info']['prot'] & 5 == 5, 'the built-in Disk.info: %s' % ls['Disk.info'])
hi, r = openf(G, FINDINPUT, 'disk.INFO')
chk(hi and readall(G, hi, 100) == ICON and mkinfo.read_icon(ICON)[0] == mkinfo.WBDISK, 'the built-in Disk.info reads as a disk icon')
ok(pkt(G, END, hi), 'Close')
err(pkt(G, DELETE, 0, name('Disk.info')), 222, 'delete the built-in Disk.info')
hi, r = openf(G, FINDOUTPUT, 'Disk.info')                 # a Workbench Snapshot writes a real one
chk(hi and write(G, hi, b'snapshot')[0] == 8, 'write a real Disk.info')
ok(pkt(G, END, hi), 'Close')
ls2 = [e['name'] for e in listdir(G, root)]
chk(ls2.count('Disk.info') == 1, 'one Disk.info once a real one is there: %s' % ls2)
hi, r = openf(G, FINDINPUT, 'Disk.info')
chk(hi and readall(G, hi, 100) == b'snapshot', 'the real Disk.info wins')
ok(pkt(G, END, hi), 'Close')
ok(pkt(G, DELETE, 0, name('Disk.info')), 'delete the real Disk.info')
hi, r = openf(G, FINDINPUT, 'Disk.info')
chk(hi and readall(G, hi, 100) == ICON, 'the built-in one is back')
ok(pkt(G, END, hi), 'Close')
A = start(0)
la = {e['name']: e for e in listdir(A, ok(pkt(A, LOCATE, 0, name(''), -2), 'Lock ALICE68:'))}
chk(la['Disk.info']['size'] == len(b'its own icon') and [e for e in la].count('Disk.info') == 1, 'a folder with its own Disk.info: only that one')
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
chk(lsd == ['letter.txt', 'Disk.info'], 'Documents: %s' % lsd)
ok(pkt(DO, EXAMINE, ok(pkt(DO, LOCATE, 0, name(''), -2), 'Lock'), FIB >> 2), 'Examine')
chk(fib()['name'] == 'Documents', 'the volume name, not the folder name')
# ---- Amiga attributes in NAME.uaem sidecars (FS-UAE / Amiberry's format) ---------------------------------
MSUB = ok(pkt(G, LOCATE, 0, name('Sub'), -2), 'Lock Sub')
chk(sorted(e['name'] for e in listdir(G, MSUB)) == ['Script', 'What?.txt', 'deep.bin'], 'Sub lists: sidecars and clutter hidden')
def exam(path):
    l = ok(pkt(G, LOCATE, 0, name(path), -2), 'Lock ' + path)
    ok(pkt(G, EXAMINE, l, FIB >> 2), 'Examine ' + path)
    f = fib(); f['comment'] = bytes(mem.r_block(FIB + 145, mem.r8(FIB + 144))).decode('latin1'); f['ticks'] = mem.r32(FIB + 140)
    ok(pkt(G, FREE, l), 'UnLock')
    return f
f = exam('Sub/Script')
chk(f['prot'] == 0x40 and f['comment'] == 'made on a PC', 'a PC-made sidecar: script bit, comment: %x %r' % (f['prot'], f['comment']))
chk((f['days'], f['mins'], f['ticks']) == (15466, 7 * 60 + 8, 9 * 50 + 5), 'its date: %s' % ((f['days'], f['mins'], f['ticks']),))
h, r = openf(G, FINDINPUT, 'Sub/What?.txt')
chk(h and readall(G, h, 100) == b'question\n', 'a %3f name read as What?.txt'); ok(pkt(G, END, h), 'Close')
for hidden in ('Sub/Script.uaem', 'Sub/._deep.bin', 'Sub/.DS_Store'):
    err(pkt(G, LOCATE, 0, name(hidden), -2), 205, 'hidden: ' + hidden)
# made on the Amiga
h, r = openf(G, FINDOUTPUT, 'Sub/new note'); write(G, h, b'v1'); ok(pkt(G, END, h), 'Close')
COMMENT = bstr('hello from the Amiga')
ok(pkt(G, 28, 0, 0, name('Sub/new note'), COMMENT), 'SetComment')
chk(exam('Sub/new note')['comment'] == 'hello from the Amiga', 'the comment reads back')
ok(pkt(G, PROTECT, 0, 0, name('Sub/new note'), 0x40), 'SetProtection: the script bit')
chk(exam('Sub/new note')['prot'] & 0xff == 0x40, 'the script bit reads back')
ok(pkt(G, PROTECT, 0, 0, name('Sub/new note'), 0x01), 'SetProtection: no delete only (FAT cannot say it)')
err(pkt(G, DELETE, 0, name('Sub/new note')), 222, 'delete-protected through the sidecar')
h, r = openf(G, FINDUPDATE, 'Sub/new note'); write(G, h, b'v2'); ok(pkt(G, END, h), 'Close')
fn = exam('Sub/new note')
chk((fn['days'], fn['mins'], fn['ticks']) == (17841, 13 * 60 + 37, 2100), 'a write brings the sidecar date up to date: %s' % fn)
ok(pkt(G, PROTECT, 0, 0, name('Sub/new note'), 0), 'SetProtection back to ----rwed')
ok(pkt(G, 28, 0, 0, name('Sub/new note'), bstr('')), 'SetComment empty')
ok(pkt(G, RENAME, 0, name('Sub/Script'), 0, name('Sub/Script2')), 'Rename a file with a sidecar')
chk(exam('Sub/Script2')['comment'] == 'made on a PC', 'the sidecar went with it')
h, r = openf(G, FINDOUTPUT, 'Sub/a*b?'); write(G, h, b'odd'); ok(pkt(G, END, h), 'Close')
chk('a*b?' in [e['name'] for e in listdir(G, MSUB)], 'a name FAT cannot hold, listed as the Amiga gave it')
h, r = openf(G, FINDOUTPUT, 'Sub/gone'); ok(pkt(G, END, h), 'Close')
ok(pkt(G, 28, 0, 0, name('Sub/gone'), bstr('bye')), 'SetComment')
ok(pkt(G, DELETE, 0, name('Sub/gone')), 'Delete a file with a sidecar')
print('sidecars: PC-made read, Amiga-made written; names escaped; clutter hidden')

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
sub_dir = subprocess.run(['mdir', '-b', '-i', img, '::/Games/Sub'], capture_output=True, text=True, env=env).stdout
chk('Script2.uaem' in sub_dir and 'Script.uaem' not in sub_dir.replace('Script2.uaem', ''), 'mtools: the sidecar renamed: %s' % sub_dir)
chk('new note.uaem' not in sub_dir and 'gone' not in sub_dir, 'mtools: sidecars removed when nothing needs them: %s' % sub_dir)
chk(mtype('Games/Sub/a%2ab%3f') == b'odd', 'mtools: a*b? stored as a%2ab%3f')
chk(mtype('Games/Sub/Script2.uaem') == b'-s--rwed 2020-05-06 07:08:09.10 made on a PC\n', 'mtools: the sidecar unchanged by the rename')
print('card: fsck.fat clean, mtools reads it back')
print('ALL OK')
