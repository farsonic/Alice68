#!/usr/bin/env python3
"""Run alice68hdf.device on a 68k emulator (machine68k / Musashi) against a
test SD card image (tools/hdf-test/mkimg.py), with a fake exec, expansion
library, FileSystem.resource and brcm-emmc.device that reads the image.

    harness_hdf.py DEVICE.hunk DEVICE.elf CARD.img HDFDIR [nofsres]

Checks the boot list the driver builds (names, priorities, filesystems), then
opens units and reads through BeginIO + the I/O task's service routine,
comparing every byte with the original hardfiles.
SPDX-License-Identifier: GPL-2.0-only"""
import struct, subprocess, sys
import machine68k as M

R = M.Register
mach = M.Machine(M.CPUType.M68020, 16384)          # 16 MB
cpu, mem, traps = mach.cpu, mach.mem, mach.traps

SYSBASE, EXPBASE, EMMC, FSRES = 0x8000, 0xA000, 0xC000, 0xD000
LOAD, DEVBASE, CONFIGDEV = 0x20000, 0x60000, 0x7F000
HEAP, HEAP_END = 0x400000, 0x800000
BUF = 0x800000
STACK = 0x200000
hunk, elf, card_path, hdfdir = sys.argv[1:5]
opts = sys.argv[5:]
no_fsres = 'nofsres' in opts
no64 = 'no64' in opts
card_ro = 'cardro' in opts                          # brcm-emmc unit0=ro
card = bytearray(open(card_path, 'rb').read())     # writes go to this copy
card0 = bytes(card)
cmdline = ''
import re

done = {'flag': False}
exc = {'hit': None}
def die(msg):
    print('FAIL:', msg); sys.exit(1)

def t_exit(op, pc):
    done['flag'] = True; cpu.end()
EXIT_ADDR = 0x1000
mem.w16(EXIT_ADDR, 0xA000 | traps.setup(t_exit))
def t_exc(op, pc):
    sp = cpu.r_reg(R.A7)
    exc['hit'] = (mem.r16(sp + 6) & 0xfff) // 4, mem.r32(sp + 2)
    done['flag'] = True; cpu.end()
EXC_ADDR = 0x1010
mem.w16(EXC_ADDR, 0xA000 | traps.setup(t_exc))
for v in range(2, 64): mem.w32(v * 4, EXC_ADDR)
mem.w32(4, SYSBASE)

def lvo(base, off, fn):
    mem.w16(base + off, 0xA000 | traps.setup(fn, auto_rts=True))
def d(r): return cpu.r_reg(r)
def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)

# ---- memory --------------------------------------------------------------
heap = {'p': HEAP}
def alloc(n):
    a = (heap['p'] + 7) & ~7
    heap['p'] = a + n
    if heap['p'] > HEAP_END: die('harness heap exhausted')
    mem.w_block(a, bytes(n))
    return a
lvo(SYSBASE, -198, lambda o, p: setd0(alloc(d(R.D0))))
lvo(SYSBASE, -210, lambda o, p: None)
lvo(SYSBASE, -132, lambda o, p: None)
lvo(SYSBASE, -138, lambda o, p: None)
for off_ in (-558, -564, -570): lvo(SYSBASE, off_, lambda o, p: None)    # semaphores (folders)

# ---- exec lists and ports --------------------------------------------------
def newlist(l):
    mem.w32(l, l + 4); mem.w32(l + 4, 0); mem.w32(l + 8, l)
def addtail(l, n):
    pred = mem.r32(l + 8)
    mem.w32(n, l + 4); mem.w32(n + 4, pred); mem.w32(pred, n); mem.w32(l + 8, n)
def remhead(l):
    n = mem.r32(l)
    if mem.r32(n) == 0: return 0
    nxt = mem.r32(n); mem.w32(l, nxt); mem.w32(nxt + 4, l)
    return n
signals = []
def putmsg(o, p):
    port, msg = d(R.A0), d(R.A1)
    mem.w8(msg + 8, 5)                                  # NT_MESSAGE
    addtail(port + 20, msg)
    signals.append((mem.r32(port + 16), mem.r8(port + 15)))
lvo(SYSBASE, -366, putmsg)
def remove(o, p):
    n = d(R.A1); succ, pred = mem.r32(n), mem.r32(n + 4)
    mem.w32(pred, succ); mem.w32(succ + 4, pred)
lvo(SYSBASE, -252, remove)
serial = []
lvo(SYSBASE, -516, lambda o, p: serial.append(chr(d(R.D0) & 0xff)))
cache_clears = {'n': 0}
lvo(SYSBASE, -636, lambda o, p: cache_clears.__setitem__('n', cache_clears['n'] + 1))
lvo(SYSBASE, -372, lambda o, p: setd0(remhead(d(R.A0) + 20)))
replied = []
lvo(SYSBASE, -378, lambda o, p: replied.append(d(R.A1)))
def createport(o, p):
    a = alloc(34); mem.w8(a + 8, 4); mem.w8(a + 15, 17); mem.w32(a + 16, 0x5000); newlist(a + 20); setd0(a)
lvo(SYSBASE, -666, createport)
lvo(SYSBASE, -672, lambda o, p: None)
def createio(o, p):
    a = alloc(d(R.D0)); mem.w32(a + 14, d(R.A0)); mem.w16(a + 18, d(R.D0)); setd0(a)
lvo(SYSBASE, -654, createio)
lvo(SYSBASE, -660, lambda o, p: None)
lvo(SYSBASE, -330, lambda o, p: setd0(16))
tasks = []
lvo(SYSBASE, -282, lambda o, p: tasks.append((d(R.A1), d(R.A2))))
lvo(SYSBASE, -414, lambda o, p: None)
lvo(SYSBASE, -552, lambda o, p: setd0(EXPBASE if mem.r_cstr(d(R.A1)) == 'expansion.library' else 0))
lvo(SYSBASE, -498, lambda o, p: setd0(FSRES if mem.r_cstr(d(R.A1)) == 'FileSystem.resource' and not no_fsres else 0))

# ---- the fake brcm-emmc.device -------------------------------------------------
CMDS = alloc(16)
for i, c in enumerate([2, 3, 0x4000, 0xc000, 0]): mem.w16(CMDS + 2 * i, c)
stats = {'reads': 0, 'bytes': 0, 'writes': 0}
def openDev(o, p):
    name, unit, io = mem.r_cstr(d(R.A0)), d(R.D0), d(R.A1)
    if name == 'brcm-emmc.device' and unit == 0:
        mem.w32(io + 20, EMMC); mem.w8(io + 31, 0); setd0(0)
    else:
        mem.w8(io + 31, 0xff); setd0(0xffffffff)
lvo(SYSBASE, -444, openDev)
lvo(SYSBASE, -450, lambda o, p: None)
def doio(o, p):
    io = d(R.A1)
    if mem.r32(io + 20) != EMMC: die('DoIO on an unknown device')
    cmd, data, length, off, act = mem.r16(io + 28), mem.r32(io + 40), mem.r32(io + 36), mem.r32(io + 44), mem.r32(io + 32)
    err = 0
    if no64 and cmd in (0x4000, 24, 0xc000, 25, 0xc001):
        err = -3
    elif cmd == 15:
        mem.w32(io + 32, 1 if card_ro else 0)
    elif cmd in (3, 25, 0xc001):
        pos = off | ((act << 32) if cmd != 3 else 0)
        if card_ro: err = 28
        elif pos % 512 or length % 512 or pos + length > len(card): err = -4
        else:
            card[pos:pos + length] = bytes(mem.r_block(data, length)); mem.w32(io + 32, length)
            stats['writes'] += 1
    elif cmd == 0x4000:
        mem.w32(data, 0); mem.w32(data + 4, 16); mem.w16(data + 8, 5); mem.w16(data + 10, 0); mem.w32(data + 12, CMDS)
        mem.w32(io + 32, 16)
    elif cmd in (2, 24, 0xc000):
        pos = off | ((act << 32) if cmd != 2 else 0)
        if pos % 512 or length % 512: err = -4
        else:
            b = bytes(card[pos:pos + length])
            if len(b) != length: err = 20
            else:
                mem.w_block(data, b); mem.w32(io + 32, length)
                stats['reads'] += 1; stats['bytes'] += length
    else:
        err = -3
    mem.w8(io + 31, err & 0xff)
    setd0(err & 0xffffffff)
lvo(SYSBASE, -456, doio)

# ---- expansion.library: config board, MakeDosNode, AddBootNode, MountList -------
newlist(EXPBASE + 74)
def bstr(s):
    a = alloc(len(s) + 4) & ~3
    a = (a + 3) & ~3
    mem.w8(a, len(s)); mem.w_block(a + 1, s.encode())
    return a >> 2
def mountlist_add(dn, pri):
    bn = alloc(20); mem.w8(bn + 8, 16); mem.w8(bn + 9, pri & 0xff); mem.w32(bn + 16, dn)
    addtail(EXPBASE + 74, bn)
existing = alloc(44); mem.w32(existing + 40, bstr('DH0'))     # an SD 0x76 partition, already mounted
mountlist_add(existing, 0)
lvo(EXPBASE, -72, lambda o, p: setd0(CONFIGDEV if d(R.A0) == 0 and d(R.D0) == 0x6d73 and d(R.D1) == 2 else 0))
dosnodes = {}
def makedosnode(o, p):
    pkt = d(R.A0)
    name, dev, unit = mem.r_cstr(mem.r32(pkt)), mem.r_cstr(mem.r32(pkt + 4)), mem.r32(pkt + 8)
    ts = mem.r32(pkt + 16)
    env = [mem.r32(pkt + 16 + 4 * i) for i in range(ts + 1)]
    dn = alloc(44); mem.w32(dn + 40, bstr(name))
    dosnodes[dn] = dict(name=name, dev=dev, unit=unit, env=env)
    setd0(dn)
lvo(EXPBASE, -144, makedosnode)
boots = []
def addbootnode(o, p):
    pri, fl, dn, cd = d(R.D0), d(R.D1), d(R.A0), d(R.A1)
    pri = pri - (1 << 32) if pri & 0x80000000 else pri
    info = dict(dosnodes[dn]); info.update(pri=pri, flags=fl, cd=cd, seglist=mem.r32(dn + 32), gv=mem.r32(dn + 36))
    boots.append(info); mountlist_add(dn, pri); setd0(1)
lvo(EXPBASE, -36, addbootnode)

# FileSystem.resource with a PFS\3 entry (patches dn_SegList and dn_GlobalVec)
newlist(FSRES + 18)
fse = alloc(62)
mem.w32(fse + 14, 0x50465303); mem.w32(fse + 18, 0x00130002); mem.w32(fse + 22, 0x180)
mem.w32(fse + 26 + 4 * 7, 0x1234); mem.w32(fse + 26 + 4 * 8, 0xffffffff)
addtail(FSRES + 18, fse)

# ---- load and start, as Emu68's ROM board does -------------------------------------
def load_hunk(path):
    h = open(path, 'rb').read()
    w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    assert w(0) == 0x3f3 and w(8) == 1
    p = 24; assert w(p) == 0x3e9
    n = w(p + 4) * 4
    code = bytearray(h[p + 8:p + 8 + n]); p += 8 + n
    relocs = []
    if w(p) == 0x3ec:
        cnt = w(p + 4); relocs = [w(p + 12 + 4 * i) for i in range(cnt)]
    for r in relocs:
        v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    return len(code)
size = load_hunk(hunk)
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
    for r, v in (regs or {}).items(): cpu.w_reg(r, v & 0xffffffff)
    cpu.w_pc(addr); cpu.w_sr(0x0000)                    # user mode, like a task
    n = 0
    while not done['flag'] and n < 5000:
        cpu.execute(100000); n += 1
    if exc['hit']:
        vec, epc = exc['hit']
        die('%s: CPU exception %d at PC $%06x (hunk offset $%x)' % (name, vec, epc, epc - LOAD))
    if not done['flag']: die('%s: did not return' % name)
    return cpu.r_reg(R.D0)

rt = LOAD + 4
assert mem.r16(rt) == 0x4afc and mem.r32(rt + 2) == rt
assert mem.r8(rt + 10) & 0x80 and mem.r8(rt + 12) == 3 and mem.r8(rt + 13) == 10, 'RTF_AUTOINIT device, priority 10'
inittab = mem.r32(rt + 22)
datasize, functab, _, initfn = (mem.r32(inittab + 4 * i) for i in range(4))
funcs = []
f = functab
while mem.r32(f) != 0xffffffff: funcs.append(mem.r32(f)); f += 4
assert len(funcs) == 6
for i, fa in enumerate(funcs):
    jt = DEVBASE - 6 * (i + 1); mem.w16(jt, 0x4ef9); mem.w32(jt + 2, fa)
r = call(initfn, {R.D0: DEVBASE, R.A0: 0, R.A6: SYSBASE}, name='Init')
if r != DEVBASE: die('Init returned %x' % r)

wr = mem.r32(DEVBASE + 44 + 4 * 16)
logtxt = bytes(mem.r_block(DEVBASE + 48 + 4 * 16, min(wr, 4096))).decode('latin1')
print('--- driver log ---\n' + logtxt.rstrip())
print('--- boot list ---')
for b in boots:
    print('  %-6s unit %d pri %4d dostype %08x seglist %x cd %x' % (b['name'], b['unit'], b['pri'], b['env'][16], b['seglist'], b['cd']))

nunits = mem.r32(DEVBASE + 40)
m = re.search(r'cmdline.txt:(.*)', logtxt)
scen = 'extra' if m and 'hdf.boot=extra' in m.group(1) else \
       'bootnb' if m and 'hdf.noboot' in m.group(1) and 'hdf.boot=raw' in m.group(1) else \
       'noboot' if m and 'hdf.noboot' in m.group(1) else \
       'boot' if m and 'hdf.boot=workbench' in m.group(1) else 'raw' if m and 'hdf.boot=RAW' in m.group(1) else \
       'off' if m and 'hdf=off' in m.group(1) else 'ro' if (m and 'hdf.readonly' in m.group(1)) else 'default'
G = [('GAMES', None, -128)]          # PFS\3: from FileSystem.resource, or loaded from games.hdf's RDB
if scen in ('default', 'ro', 'noboot'):
    # everything mounted, nothing bootable; names taken (the SD card's DH0) become HF<unit><part>;
    # extra.hdf's volume SDWork is the SD card partition's: not mounted
    order = ['games.hdf', 'Workbench 3.1.hdf', 'frag.hdf', 'raw.hdf', 'extra.hdf']
    want = [('DH1', 0, -128), ('GAMES', 0, -128), ('HF10', 1, -128), ('FR0', 2, -128), ('DH2', 3, -128)]
elif scen == 'boot':
    # Workbench boots as DH0 (the SD card's DH0 and its volume SDWork gone); extra's SDWork then mounts
    order = ['Workbench 3.1.hdf', 'games.hdf', 'frag.hdf', 'raw.hdf', 'extra.hdf']
    want = [('DH0', 0, 5), ('DH1', 1, -128), ('GAMES', 1, -128), ('FR0', 2, -128), ('DH2', 3, -128), ('DH5', 4, -128)]
elif scen in ('raw', 'bootnb'):
    order = ['raw.hdf', 'games.hdf', 'Workbench 3.1.hdf', 'frag.hdf', 'extra.hdf']
    want = [('DH0', 0, 0), ('DH1', 1, -128), ('GAMES', 1, -128), ('HF20', 2, -128), ('FR0', 3, -128), ('DH5', 4, -128)]
elif scen == 'extra':
    # extra.hdf (DH5, volume SDWork) boots: the SD card's drive with that volume (DH0) is not mounted,
    # so Workbench gets DH0
    order = ['extra.hdf', 'games.hdf', 'Workbench 3.1.hdf', 'frag.hdf', 'raw.hdf']
    want = [('DH5', 0, 0), ('DH1', 1, -128), ('GAMES', 1, -128), ('DH0', 2, -128), ('FR0', 3, -128), ('DH2', 4, -128)]
else:
    order, want = [], []
summary = {'default': (0, 4, 1), 'ro': (0, 4, 1), 'noboot': (0, 4, 1), 'boot': (1, 4, 0), 'raw': (1, 4, 0),
           'bootnb': (1, 4, 0), 'extra': (1, 4, 0), 'off': (0, 0, 0)}[scen]
ms = re.search(r'\[alice68hdf\] hardfiles: (\d+) bootable, (\d+) non-bootable, (\d+) left out', logtxt)
if not ms or tuple(int(x) for x in ms.groups()) != summary: die('%s: summary line %s, want %s' % (scen, ms and ms.groups(), summary))
if ms.group(0) not in ''.join(serial): die('summary line not sent through RawPutChar')
got = [(b['name'], b['unit'], b['pri']) for b in boots]
if got != want: die('%s: boot list %s, want %s' % (scen, got, want))
if any(b['cd'] != CONFIGDEV or b['flags'] != 1 or b['dev'] != 'alice68hdf.device' for b in boots): die('boot node device/flags/configdev')
g = [b for b in boots if b['name'] == 'GAMES']
if g and not no_fsres and (g[0]['seglist'] != 0x1234 or g[0]['gv'] != 0xffffffff):
    die('PFS\\3 not patched from FileSystem.resource (19.2 there, 18.0 in the RDB)')
if g and no_fsres:
    # loaded from the RDB: a seglist of 3 hunks, relocated, the code flushed
    seg, hunks = g[0]['seglist'], []
    while seg:
        link = seg << 2
        hunks.append((link + 4, mem.r32(link - 4)))         # data, allocated size
        seg = mem.r32(link)
    if len(hunks) != 3 or [sz for _, sz in hunks] != [8 * 4 + 8, 4 * 4 + 8, 16 * 4 + 8]: die('loaded seglist %s' % hunks)
    c, dt, bs = (a for a, _ in hunks)
    code = [mem.r32(c + 4 * i) for i in range(8)]
    data = [mem.r32(dt + 4 * i) for i in range(4)]
    if code[0] != dt + 4 or code[1] != 0x11111111 or code[2] != c + 16 or code[3] != 0x22222222: die('code hunk relocation %s' % [hex(x) for x in code])
    if data[1] != bs + 12 or data[0] != 0xaaaa0000 or any(mem.r32(bs + 4 * i) for i in range(16)): die('data/bss hunk %s' % [hex(x) for x in data])
    if g[0]['gv'] != 0xffffffff: die('dn_GlobalVec from the FSHD')
    if cache_clears['n'] < 1: die('no CacheClearU after loading code')
    print('PFS\\3 loaded from the RDB: 3 hunks, relocations right, caches cleared')
if nunits != len(order): die('%s: %d units, want %d' % (scen, nunits, len(order)))
if scen == 'off':
    if tasks: die('hdf=off started a task')
    print('hdf=off: no units, no boot nodes, no task'); print('ALL OK'); sys.exit(0)
if len(tasks) != 1: die('no I/O task')
task, initpc = tasks[0]
if mem.r32(task + 88) != DEVBASE or initpc != syms['task_entry']: die('task set up wrong')
# the SD card's DH0 is still mounted unless a hardfile took its name over
ml, nodes = EXPBASE + 74, []
n_ = mem.r32(ml)
while mem.r32(n_): nodes.append(n_); n_ = mem.r32(n_)
sd_there = any(mem.r32(x + 16) == existing for x in nodes)
if 'loaded from the RDB' in logtxt and not no_fsres: die('loaded a filesystem older than FileSystem.resource\'s')
if '[alice68hdf] ' not in ''.join(serial) or 'units ready' not in ''.join(serial): die('log not sent through RawPutChar')
if sd_there != (scen in ('default', 'ro', 'noboot')): die('%s: the SD card DH0 is %s the mount list' % (scen, 'in' if sd_there else 'not in'))
print('%s: boot list OK, SD DH0 %s; %d units; task OK; %d card reads at init' % (scen, 'kept' if sd_there else 'replaced', nunits, stats['reads']))

# ---- I/O through BeginIO and the task's service routine -------------------------
VEC = lambda off: DEVBASE + off
def open_unit(unit):
    io = alloc(48)
    call(VEC(-6), {R.A6: DEVBASE, R.A1: io, R.D0: unit, R.D1: 0}, name='Open')
    mem.w32(io + 20, DEVBASE)
    return io
cardio = alloc(48); mem.w32(cardio + 20, EMMC)
cardctx = alloc(8); mem.w32(cardctx, DEVBASE); mem.w32(cardctx + 4, cardio)

def io_req(io, cmd, off, length, hi=0, quick=1, data=BUF):
    mem.w16(io + 28, cmd); mem.w8(io + 30, quick); mem.w8(io + 31, 0x55)
    mem.w32(io + 32, hi); mem.w32(io + 36, length); mem.w32(io + 40, data); mem.w32(io + 44, off)
    replied.clear()
    call(VEC(-30), {R.A6: DEVBASE, R.A1: io}, name='BeginIO %x' % cmd)
    if not (mem.r8(io + 30) & 1):                     # queued for the task
        call(syms['hdf_service'], args=(DEVBASE, cardctx), name='service')
        if io not in replied: die('request %x not replied' % cmd)
    return mem.r8s(io + 31), mem.r32(io + 32)

def orig(name, blk, n):
    with open('%s/%s' % (hdfdir, name), 'rb') as f:
        f.seek(blk * 512); return f.read(n * 512)

checks = 0
U = {name: i for i, name in enumerate(order)}
wb, fr = U['Workbench 3.1.hdf'], U['frag.hdf']
io1 = open_unit(wb)
if mem.r8(io1 + 31): die('Open Workbench unit failed')
e, act = io_req(io1, 2, 1000 * 512, 8 * 512)
if e or act != 4096 or bytes(mem.r_block(BUF, 4096)) != orig('Workbench 3.1.hdf', 1000, 8): die('CMD_READ: err %d act %d' % (e, act))
e, act = io_req(io1, 24, 5 * 512, 512, hi=0)
if e or bytes(mem.r_block(BUF, 512)) != orig('Workbench 3.1.hdf', 5, 1): die('TD_READ64')
e, act = io_req(io1, 0xc000, 10239 * 512, 512)
if e or bytes(mem.r_block(BUF, 512)) != orig('Workbench 3.1.hdf', 10239, 1): die('NSCMD_TD_READ64 last block')
e, act = io_req(io1, 2, 10240 * 512, 512)
if e != -5: die('read past the end: err %d, want -5' % e)
e, act = io_req(io1, 2, 100, 512)
if e != -4: die('unaligned read: err %d, want -4' % e)
geo = alloc(32)
mem.w16(io1 + 28, 22); mem.w8(io1 + 30, 1); mem.w32(io1 + 36, 32); mem.w32(io1 + 40, geo)
call(VEC(-30), {R.A6: DEVBASE, R.A1: io1}, name='GETGEOMETRY')
g = [mem.r32(geo + 4 * i) for i in range(6)]
if g != [512, 10240, 160, 64, 2, 32]: die('TD_GETGEOMETRY %s' % g)
io2 = open_unit(fr)
e, act = io_req(io2, 2, 0, 4096 * 512)
if e or bytes(mem.r_block(BUF, 4096 * 512)) != orig('frag.hdf', 0, 4096): die('whole fragmented file')
checks += 7

# ---- writes ----------------------------------------------------------------------
ro = scen == 'ro' or card_ro
e, act = io_req(io1, 15, 0, 0)
if e or act != (1 if ro else 0): die('TD_PROTSTATUS %d, want %d' % (act, 1 if ro else 0))
pat = bytes((0xa5 ^ (i * 7) ^ (i >> 9)) & 0xff for i in range(512 * 400))
WBUF = BUF + 0x300000
mem.w_block(WBUF, pat)
e1, _ = io_req(io1, 3, 2000 * 512, 16 * 512, data=WBUF)                      # CMD_WRITE
e2, _ = io_req(io2, 25, 50 * 512, 400 * 512, hi=0, data=WBUF)                # TD_WRITE64 across frag's pieces
e3, _ = io_req(io1, 0xc001, 10239 * 512, 512, data=WBUF)                     # NSCMD_TD_WRITE64, the last block
e4, _ = io_req(io1, 3, 10239 * 512, 1024, data=WBUF)                        # past the end
if ro:
    if (e1, e2, e3) != (28, 28, 28): die('read-only: writes answered %s, want 28' % ((e1, e2, e3),))
    if bytes(card) != card0: die('read-only: the card changed')
    print('read-only: writes refused (28), card unchanged')
else:
    if (e1, e2, e3) != (0, 0, 0): die('writes failed: %s' % ((e1, e2, e3),))
    if e4 != -5: die('write past the end: err %d, want -5' % e4)
    e, act = io_req(io1, 2, 2000 * 512, 16 * 512)
    if e or bytes(mem.r_block(BUF, 16 * 512)) != pat[:16 * 512]: die('Workbench write read-back')
    e, act = io_req(io2, 2, 0, 4096 * 512)
    want_frag = orig('frag.hdf', 0, 50) + pat + orig('frag.hdf', 450, 4096 - 450)
    if e or bytes(mem.r_block(BUF, 4096 * 512)) != want_frag: die('frag write read-back (whole file)')
    changed = sum(1 for s_ in range(0, len(card), 512) if card[s_:s_ + 512] != card0[s_:s_ + 512])
    if changed != 16 + 400 + 1: die('%d sectors changed on the card, want 417' % changed)
    for other in [n for n in order if n not in ('Workbench 3.1.hdf', 'frag.hdf')]:
        ioo = open_unit(U[other])
        blocks = len(open('%s/%s' % (hdfdir, other), 'rb').read()) // 512
        e, act = io_req(ioo, 2, 0, blocks * 512)
        if e or bytes(mem.r_block(BUF, blocks * 512)) != orig(other, 0, blocks): die('%s changed by writes to other files' % other)
    print('writes OK: %d sectors changed on the card (417), other hardfiles untouched' % changed)
checks += 4
io9 = alloc(48)
call(VEC(-6), {R.A6: DEVBASE, R.A1: io9, R.D0: 9, R.D1: 0}, name='Open 9')
if mem.r8(io9 + 31) == 0: die('Open of a unit that does not exist succeeded')
print('I/O OK: %d checks, %d card reads, %d writes' % (checks + 1, stats['reads'], stats['writes']))
print('ALL OK')
