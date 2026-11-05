#!/usr/bin/env python3
"""Run alice68-usb.device on a 68k emulator (machine68k / Musashi) with a
faked exec + expansion.library + kernel mailbox board, replaying what Neptune
does when it brings the driver online. Any CPU exception (Line-F, address
error, ...) is reported with its PC and the hunk offset.
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M

R = M.Register
mach = M.Machine(M.CPUType.M68020, 4096)          # 4 MB
cpu, mem, traps = mach.cpu, mach.mem, mach.traps

SYSBASE, EXPBASE = 0x8000, 0xA000
LOAD, DEVBASE = 0x20000, 0x40000
BOARD, CONFIGDEV = 0x60000, 0x7F000
IOS, BUFS, TAGS = 0x80000, 0x90000, 0x9F000
STACK = 0x200000
PT_OFF_REQS, PT_MAGIC, PT_VERSION = 0x200, 0x45415550, 2

log = []
done = {'flag': False}
exc = {'hit': None}

def die(msg):
    print('FAIL:', msg); sys.exit(1)

# ---- traps ---------------------------------------------------------------
def t_exit(op, pc):
    done['flag'] = True
    cpu.end()
EXIT_TID = traps.setup(t_exit)
EXIT_ADDR = 0x1000
mem.w16(EXIT_ADDR, 0xA000 | EXIT_TID)

def t_exc(op, pc):
    # stack: SR(2) PC(4) fmt(2)
    sp = cpu.r_reg(R.A7)
    sr = mem.r16(sp); epc = mem.r32(sp + 2); vec = mem.r16(sp + 6) & 0xfff
    exc['hit'] = (vec // 4, epc, sr)
    done['flag'] = True
    cpu.end()
EXC_TID = traps.setup(t_exc)
EXC_ADDR = 0x1010
mem.w16(EXC_ADDR, 0xA000 | EXC_TID)
for v in range(2, 64):
    mem.w32(v * 4, EXC_ADDR)

ints = []          # (intnum, addr of struct Interrupt)
replied = []
disable = {'n': 0}

def lvo(base, off, fn):
    tid = traps.setup(fn, auto_rts=True)
    mem.w16(base + off, 0xA000 | tid)

def d(r): return cpu.r_reg(r)
def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)

lvo(SYSBASE, -120, lambda o, p: disable.__setitem__('n', disable['n'] + 1))
lvo(SYSBASE, -126, lambda o, p: disable.__setitem__('n', disable['n'] - 1))
lvo(SYSBASE, -132, lambda o, p: None)
lvo(SYSBASE, -138, lambda o, p: None)
lvo(SYSBASE, -168, lambda o, p: ints.append((d(R.D0), d(R.A1))))
lvo(SYSBASE, -174, lambda o, p: None)
lvo(SYSBASE, -210, lambda o, p: None)
lvo(SYSBASE, -252, lambda o, p: None)
lvo(SYSBASE, -378, lambda o, p: replied.append(d(R.A1)))
lvo(SYSBASE, -414, lambda o, p: None)
def openlib(o, p):
    name = mem.r_cstr(d(R.A1))
    setd0(EXPBASE if name == 'expansion.library' else 0)
lvo(SYSBASE, -552, openlib)
def findcd(o, p):
    old, man, prod = d(R.A0), d(R.D0), d(R.D1)
    setd0(CONFIGDEV if old == 0 and man == 0x6d73 and prod == 0x6a else 0)
lvo(EXPBASE, -72, findcd)
mem.w32(4, SYSBASE)

# ---- load the hunk --------------------------------------------------------
def load_hunk(path):
    h = open(path, 'rb').read()
    w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    assert w(0) == 0x3f3 and w(4) == 0 and w(8) == 1
    p = 24
    assert w(p) == 0x3e9
    n = w(p + 4) * 4
    code = bytearray(h[p + 8:p + 8 + n]); p += 8 + n
    relocs = []
    if w(p) == 0x3ec:
        cnt = w(p + 4); assert w(p + 8) == 0
        relocs = [w(p + 12 + 4 * i) for i in range(cnt)]
        p += 12 + 4 * cnt + 4
    assert w(p) == 0x3f2
    for r in relocs:
        v = struct.unpack('>I', code[r:r + 4])[0]
        code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    return len(code)

size = load_hunk(sys.argv[1])

def call(addr, regs, name):
    done['flag'] = False
    sp = STACK - 16
    mem.w32(sp, EXIT_ADDR)
    cpu.w_reg(R.A7, sp)
    for r, v in regs.items(): cpu.w_reg(r, v & 0xffffffff)
    cpu.w_pc(addr)
    cpu.w_sr(0x2000)
    n = 0
    while not done['flag'] and n < 200:
        cpu.execute(100000); n += 1
    if exc['hit']:
        vec, epc, sr = exc['hit']
        names = {2: 'bus error', 3: 'address error', 4: 'illegal instruction', 10: 'Line-A', 11: 'Line-F', 8: 'privilege'}
        die('%s: CPU exception %d (%s) at PC $%06x (hunk offset $%x)' % (name, vec, names.get(vec, '?'), epc, epc - LOAD))
    if not done['flag']: die('%s: did not return' % name)
    if disable['n'] != 0: die('%s: Disable/Enable unbalanced (%d)' % (name, disable['n']))
    return cpu.r_reg(R.D0)

# ---- InitResident (RTF_AUTOINIT), as exec does --------------------------
rt = LOAD + 4
assert mem.r16(rt) == 0x4afc and mem.r32(rt + 2) == rt, 'ROMTag not at offset 4 / bad matchtag'
assert mem.r8(rt + 10) & 0x80, 'not RTF_AUTOINIT'
inittab = mem.r32(rt + 22)
datasize, functab, datatab, initfn = (mem.r32(inittab + 4 * i) for i in range(4))
print('ROMTag OK: %s / %s, data size %d' % (mem.r_cstr(mem.r32(rt + 14)), mem.r_cstr(mem.r32(rt + 18)).strip(), datasize))
funcs = []
f = functab
while mem.r32(f) != 0xffffffff:
    funcs.append(mem.r32(f)); f += 4
assert len(funcs) == 6, 'expected 6 vectors, got %d' % len(funcs)
for i, fa in enumerate(funcs):
    assert LOAD <= fa < LOAD + size, 'vector %d outside the hunk: %x' % (i, fa)
    jt = DEVBASE - 6 * (i + 1)
    mem.w16(jt, 0x4ef9); mem.w32(jt + 2, fa)
mem.w16(DEVBASE + 16, 36); mem.w16(DEVBASE + 18, datasize)     # NegSize, PosSize
r = call(initfn, {R.D0: DEVBASE, R.A0: 0x1234, R.A6: SYSBASE}, 'Init')
if r != DEVBASE: die('Init returned %x' % r)
print('Init OK, name', mem.r_cstr(mem.r32(DEVBASE + 10)))

# ---- the board, as the kernel leaves it ----------------------------------
mem.w32(BOARD + 0, PT_MAGIC); mem.w32(BOARD + 4, PT_VERSION); mem.w32(BOARD + 8, 8); mem.w32(BOARD + 12, 32)
mem.w32(CONFIGDEV + 32, BOARD)

VEC = lambda off: DEVBASE + off
def open_(io, unit):
    return call(VEC(-6), {R.A6: DEVBASE, R.A1: io, R.D0: unit, R.D1: 0}, 'Open')

io0 = IOS
open_(io0, 0)
if mem.r8(io0 + 31) != 0: die('Open: io_Error %d, diag %d' % (mem.r8s(io0 + 31), mem.r32(DEVBASE + 34)))
print('Open OK; interrupt servers:', [(n, hex(a)) for n, a in ints])
assert {n for n, a in ints} == {3, 5}

# ---- requests ----------------------------------------------------------------
IOSZ = 96
def mkio(i, cmd, addr=0, ep=0, d_=0, length=0, data=0, setup=None, flags=0):
    a = IOS + IOSZ * (i + 1)
    mem.w_block(a, bytes(IOSZ))
    mem.w32(a + 20, DEVBASE); mem.w32(a + 24, mem.r32(io0 + 24))
    mem.w16(a + 28, cmd); mem.w8(a + 30, 1)              # IOF_QUICK, as DoIO
    mem.w16(a + 32, flags); mem.w16(a + 36, d_); mem.w16(a + 38, addr); mem.w16(a + 40, ep); mem.w16(a + 42, 64)
    mem.w32(a + 48, length); mem.w32(a + 52, data)
    if setup:
        rt_, rq, val, idx, ln = setup
        mem.w8(a + 62, rt_); mem.w8(a + 63, rq)
        for off, v in ((64, val), (66, idx), (68, ln)): mem.w16(a + off, ((v & 0xff) << 8) | (v >> 8))   # little-endian words
    return a

def beginio(a, what):
    replied.clear()
    call(VEC(-30), {R.A6: DEVBASE, R.A1: a}, 'BeginIO ' + what)
    quick = mem.r8(a + 30) & 1
    err = mem.r8s(a + 31)
    return quick, err, list(replied)

def irq():
    for n, a in ints:
        replied_before = len(replied)
        call(mem.r32(a + 18), {R.A1: mem.r32(a + 14), R.A6: SYSBASE}, 'interrupt server %d' % n)

UHCMD = lambda k: 9 + k
# QUERYDEVICE
tags = [(0x80004711 + 0x11, TAGS + 0x100), (0x80004711 + 0x20, TAGS + 0x104), (0x80004711 + 0x21, TAGS + 0x108),
        (0x80004711 + 0x24, TAGS + 0x10c), (0x80004711 + 0x25, TAGS + 0x110), (0, 0)]
for i, (t, v) in enumerate(tags): mem.w32(TAGS + 8 * i, t); mem.w32(TAGS + 8 * i + 4, v)
a = mkio(0, UHCMD(0), data=TAGS)
q, e, rp = beginio(a, 'QUERYDEVICE')
qd = (mem.r32(TAGS + 0x104), mem.r32(TAGS + 0x108))
if qd != (0x200, 1): die('QUERYDEVICE values %s' % (qd,))
print('QUERYDEVICE: err %d quick %d product "%s" drv %x caps %x align %d' % (e, q, mem.r_cstr(mem.r32(TAGS + 0x100)), mem.r32(TAGS + 0x104), mem.r32(TAGS + 0x108), mem.r32(TAGS + 0x110)))
a = mkio(0, UHCMD(1)); q, e, rp = beginio(a, 'USBRESET'); print('USBRESET err', e)

def ctrl(addr, setup, length=64, what='', dirin=True):
    a = mkio(2, UHCMD(3), addr=addr, d_=0, length=length, data=BUFS, setup=setup)
    mem.w_block(BUFS, bytes(256))
    q, e, rp = beginio(a, what)
    return a, q, e, mem.r32(a + 44), mem.r_block(BUFS, 32)

# root hub enumeration, as hub.class does
for st, what in [((0x80, 6, 0x100, 0, 8), 'GET_DESCRIPTOR dev 8'), ((0x00, 5, 1, 0, 0), 'SET_ADDRESS 1'),
                 ((0x80, 6, 0x100, 0, 18), 'GET_DESCRIPTOR dev'), ((0x80, 6, 0x200, 0, 9), 'GET_DESCRIPTOR cfg 9'),
                 ((0x80, 6, 0x200, 0, 25), 'GET_DESCRIPTOR cfg'), ((0x00, 9, 1, 0, 0), 'SET_CONFIGURATION'),
                 ((0xa0, 6, 0x2900, 0, 11), 'GET hub descriptor')]:
    addr = 0 if 'SET_ADDRESS' in what or what == 'GET_DESCRIPTOR dev 8' else 1
    a, q, e, act, data = ctrl(addr, st, what=what)
    print('  root hub %-22s err %d actual %d %s' % (what, e, act, data[:act].hex()))
    if e: die(what)
for p in range(1, 9):
    a, q, e, act, data = ctrl(1, (0x23, 3, 8, p, 0), what='SetPortFeature POWER')
a = mkio(3, UHCMD(5), addr=1, ep=1, d_=2, length=2, data=BUFS + 0x200)
q, e, rp = beginio(a, 'INTXFER root hub')
print('root hub INTXFER: quick %d (0 = queued) replied %s' % (q, rp))
rhint = a

# the kernel offers a device on port 1
mem.w32(BOARD + 64 + 0, 1); mem.w32(BOARD + 64 + 4, 3); mem.w32(BOARD + 64 + 8, 1)
replied.clear(); irq()
print('after port attach + interrupt: replied', [hex(x) for x in replied], 'bitmap', mem.r_block(BUFS + 0x200, 2).hex())
if rhint not in replied: die('root hub change not reported')
a, q, e, act, data = ctrl(1, (0xa3, 0, 0, 1, 4), what='GetPortStatus 1'); print('  port 1 status', data[:4].hex())
if data[:4] != bytes([0x01, 0x05, 0x01, 0x00]): die('port status after connect: %s' % data[:4].hex())
ctrl(1, (0x23, 3, 4, 1, 0), what='SetPortFeature RESET')
a, q, e, act, data = ctrl(1, (0xa3, 0, 0, 1, 4), what='GetPortStatus 1'); print('  port 1 after reset', data[:4].hex())
if data[:4] != bytes([0x03, 0x05, 0x11, 0x00]): die('port status after reset: %s' % data[:4].hex())
ctrl(1, (0x23, 1, 20, 1, 0), what='ClearPortFeature C_RESET')

# the device at address 0: posted to the mailbox
a = mkio(4, UHCMD(3), addr=0, length=8, data=BUFS + 0x300, setup=(0x80, 6, 0x100, 0, 8))
q, e, rp = beginio(a, 'device GET_DESCRIPTOR')
r0 = BOARD + PT_OFF_REQS
print('device GET_DESCRIPTOR: quick %d; mailbox slot 0 state %d port %d type %d len %d data %x setup %s' % (
    q, mem.r32(r0), mem.r32(r0 + 4), mem.r32(r0 + 8), mem.r32(r0 + 20), mem.r32(r0 + 24), mem.r_block(r0 + 28, 8).hex()))
if q or mem.r32(r0) != 1: die('not submitted')
mem.w_block(BUFS + 0x300, bytes([18, 1, 0, 2, 0, 0, 0, 64])); mem.w32(r0 + 44, 8); mem.w32(r0 + 48, 0); mem.w32(r0, 3)
replied.clear(); irq()
print('kernel DONE + interrupt: replied', [hex(x) for x in replied], 'actual', mem.r32(a + 44), 'slot state', mem.r32(r0))
if a not in replied: die('completion not replied')

a, q, e, act, data = ctrl(0, (0x00, 5, 2, 0, 0), what='device SET_ADDRESS 2'); print('device SET_ADDRESS: err', e)
a = mkio(5, UHCMD(6), addr=2, ep=2, d_=1, length=31, data=BUFS + 0x400)
q, e, rp = beginio(a, 'BULKXFER OUT'); print('bulk OUT: quick %d slot0 state %d port %d ep %d dir %d len %d seq %d' % (
    q, mem.r32(r0), mem.r32(r0 + 4), mem.r32(r0 + 12), mem.r32(r0 + 16), mem.r32(r0 + 20), mem.r32(r0 + 56)))
mem.w32(r0 + 44, 31); mem.w32(r0, 3); replied.clear(); irq()
if a not in replied: die('bulk not replied')
a = mkio(6, UHCMD(6), addr=2, ep=1, d_=2, length=512, data=BUFS + 0x800)
beginio(a, 'BULKXFER IN')
call(VEC(-36), {R.A6: DEVBASE, R.A1: a}, 'AbortIO')
print('AbortIO: cancel flag', mem.r32(r0 + 52))
mem.w32(r0 + 48, 0xfffffffe); mem.w32(r0, 3); replied.clear(); irq()
print('aborted reply err', mem.r8s(a + 31))
if mem.r8s(a + 31) != -2: die('abort error')
if mem.r_block(BUFS, 1) is None: pass
print('ALL OK')
