#!/usr/bin/env python3
"""alice68net.device's open and close on a 68k emulator (machine68k / Musashi), with a fake
exec, expansion.library, dos.library and network board: a unit the kernel does not serve
(eth=off: unit 1 absent) must be refused at once, every time, with one line in the log and
nothing kept (no memory, no process, lib_OpenCnt unchanged); a CloseDevice after a refused
open changes nothing; unit 0 (WiFi) is still accepted; no such unit, refused.

    harness_alice68net.py amiga/out/alice68net.device
SPDX-License-Identifier: GPL-2.0-only"""
import os, struct, subprocess, sys, tempfile
import machine68k as M

R = M.Register
mach = M.Machine(M.CPUType.M68020, 4096)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYSBASE, EXPBASE, DOSBASE = 0x8000, 0xA000, 0xC000
LOAD, DEVBASE, BOARD, CONFIGDEV, IOS, HEAP = 0x20000, 0x40000, 0x60000, 0x7F000, 0x80000, 0x100000
STACK = 0x200000
AMIGA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def die(m): print('FAIL:', m); sys.exit(1)

# ---- the board's layout, from the 68k compiler ----
src = ('#include <stddef.h>\n#include "amiga.h"\n#include "../third_party/Emu68/src/alice/alice_net.h"\n'
       'char o_magic[offsetof(struct an_hdr, magic) + 1]; char o_version[offsetof(struct an_hdr, version) + 1];\n'
       'char o_kind[offsetof(struct an_unit, kind) + 1];\n')
with tempfile.TemporaryDirectory() as td:
    open(td + '/o.c', 'w').write(src)
    subprocess.run(['m68k-linux-gnu-gcc', '-m68020', '-fcommon', '-c', '-I', AMIGA, '-o', td + '/o.o', td + '/o.c'], check=True)
    nm = subprocess.run(['m68k-linux-gnu-nm', '-S', td + '/o.o'], capture_output=True, text=True, check=True).stdout
O = {l.split()[3][2:]: int(l.split()[1], 16) - 1 for l in nm.splitlines() if len(l.split()) == 4 and l.split()[3].startswith('o_')}
def unit_area(u): return BOARD + 0x400 + u * 0x800

# ---- the fake system ----
done = {'f': False}; exc = {'hit': None}
def t_exit(op, pc): done['f'] = True; cpu.end()
EXIT = 0x1000; mem.w16(EXIT, 0xA000 | traps.setup(t_exit))
def t_exc(op, pc):
    sp = cpu.r_reg(R.A7); exc['hit'] = ((mem.r16(sp + 6) & 0xfff) // 4, mem.r32(sp + 2)); done['f'] = True; cpu.end()
EXC = 0x1010; mem.w16(EXC, 0xA000 | traps.setup(t_exc))
for v in range(2, 64): mem.w32(v * 4, EXC)
mem.w32(4, SYSBASE)
def d(r): return cpu.r_reg(r)
def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)
def lvo(b, off, fn): mem.w16(b + off, 0xA000 | traps.setup(fn, auto_rts=True))
def unk(op, pc): die('library call the harness does not have, at $%x (hunk offset $%x)' % (pc, pc - LOAD))
U = traps.setup(unk, auto_rts=True)
for b in (SYSBASE, EXPBASE, DOSBASE):
    for off in range(6, 900, 6): mem.w16(b - off, 0xA000 | U)
st = {'allocs': 0, 'procs': 0, 'dos': 0, 'board': True}
serial = []
heap = {'p': HEAP}
def allocmem(o, p):
    a = (heap['p'] + 7) & ~7; heap['p'] = a + d(R.D0); st['allocs'] += 1; setd0(a)
lvo(SYSBASE, -198, allocmem)
lvo(SYSBASE, -210, lambda o, p: st.__setitem__('allocs', st['allocs'] - 1))
for off in (-120, -126, -132, -138, -558, -564, -570, -414): lvo(SYSBASE, off, lambda o, p: None)
lvo(SYSBASE, -516, lambda o, p: serial.append(chr(d(R.D0) & 0xff)))
lvo(SYSBASE, -294, lambda o, p: setd0(0x5000))                      # FindTask
lvo(SYSBASE, -306, lambda o, p: setd0(0))                           # SetSignal
def openlib(o, p):
    name = mem.r_cstr(d(R.A1))
    if name == 'dos.library': st['dos'] += 1
    setd0({'expansion.library': EXPBASE, 'dos.library': DOSBASE}.get(name, 0))
lvo(SYSBASE, -552, openlib)
lvo(EXPBASE, -72, lambda o, p: setd0(CONFIGDEV if st['board'] and d(R.A0) == 0 else 0))   # FindConfigDev
def newproc(o, p): st['procs'] += 1; setd0(0)                       # CreateNewProc: fails
lvo(DOSBASE, -498, newproc)
mem.w32(CONFIGDEV + 32, BOARD)                                      # cd_BoardAddr

# ---- load the device (one code hunk + RELOC32), InitResident ----
h = open(sys.argv[1], 'rb').read()
w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
n = w(28) * 4; code = bytearray(h[32:32 + n]); q = 32 + n
if w(q) == 0x3ec:
    for i in range(w(q + 4)):
        r = w(q + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
mem.w_block(LOAD, bytes(code))
def call(addr, regs, name):
    done['f'] = False
    cpu.w_reg(R.A7, STACK - 16); mem.w32(STACK - 16, EXIT)
    for r_, v in regs.items(): cpu.w_reg(r_, v & 0xffffffff)
    cpu.w_pc(addr); cpu.w_sr(0x2000)
    for _ in range(200):
        if done['f']: break
        cpu.execute(100000)
    if exc['hit']: die('%s: CPU exception %d at hunk offset $%x' % (name, exc['hit'][0], exc['hit'][1] - LOAD))
    if not done['f']: die('%s: did not return' % name)
    return cpu.r_reg(R.D0)
rt = LOAD + 4
if mem.r16(rt) != 0x4afc: die('no RomTag at offset 4')
inittab = mem.r32(rt + 22)
datasize, functab, _, initfn = (mem.r32(inittab + 4 * i) for i in range(4))
mem.w16(DEVBASE + 16, 36); mem.w16(DEVBASE + 18, datasize)
if call(initfn, {R.D0: DEVBASE, R.A0: 0x1234, R.A6: SYSBASE}, 'Init') != DEVBASE: die('Init')
print('Init OK:', mem.r_cstr(mem.r32(DEVBASE + 24)).strip())
OPEN, CLOSE = mem.r32(functab), mem.r32(functab + 4)

def board(kind0, kind1):
    mem.w_block(BOARD, bytes(0x1400))
    mem.w32(BOARD + O['magic'], 0x4136384e); mem.w32(BOARD + O['version'], 1)
    mem.w32(unit_area(0) + O['kind'], kind0); mem.w32(unit_area(1) + O['kind'], kind1)
def opendev(unit, io):
    mem.w_block(io, bytes(48)); mem.w32(io + 24, 0xDEAD0000)          # io_Unit: rubbish from the caller
    del serial[:]
    call(OPEN, {R.A6: DEVBASE, R.A1: io, R.D0: unit, R.D1: 0}, 'Open')
    return mem.r8(io + 31), mem.r32(io + 24), ''.join(serial)
def opencnt(): return mem.r16(DEVBASE + 32)
checks = [0]
def chk(c, m):
    if not c: die(m)
    checks[0] += 1

# 1. eth=off: unit 1 absent, ten times
board(1, 0)
IO = IOS
for k in range(10):
    err, unit, log = opendev(1, IO)
    chk(err == 0xff and unit == 0, 'unit 1 off: io_Error %d io_Unit %x' % (err, unit))
    chk('[alice68net] unit 1: Ethernet is off in the kernel (eth=off in cmdline.txt): open refused' in log, 'the log line: %r' % log)
chk(opencnt() == 0 and st['allocs'] == 0 and st['procs'] == 0 and st['dos'] == 0,
    'a refused open kept something: OpenCnt %d, allocations %d, processes %d, dos opens %d' % (opencnt(), st['allocs'], st['procs'], st['dos']))
print('eth=off: unit 1 refused 10 times, one log line each, nothing kept')
# 2. CloseDevice after the refused open, with a stale io_Unit
GUARD = 0x70000
mem.w_block(GUARD, bytes(64)); mem.w16(GUARD + 34, 7)               # unit_OpenCnt there must stay 7
mem.w32(IO + 24, GUARD)
call(CLOSE, {R.A6: DEVBASE, R.A1: IO}, 'Close')
chk(mem.r16(GUARD + 34) == 7 and opencnt() == 0, 'a close after a refused open changed memory or the open count')
mem.w32(IO + 24, 0)
call(CLOSE, {R.A6: DEVBASE, R.A1: IO}, 'Close')
chk(opencnt() == 0, 'close with io_Unit NULL')
print('CloseDevice after a refused open: nothing changed')
# 3. unit 0 (WiFi) is accepted: it goes on to start the process (which this fake refuses)
err, unit, log = opendev(0, IO)
chk(st['procs'] == 1 and err == 0xff and 'unit 0: the driver\'s process could not start' in log and opencnt() == 0,
    'unit 0: procs %d err %d log %r opencnt %d' % (st['procs'], err, log, opencnt()))
print('unit 0 (WiFi) while unit 1 is off: accepted (it starts the process)')
# 4. no such unit, no board
err, unit, log = opendev(2, IO)
chk(err == 0xff and 'unit 2: no such unit' in log, 'unit 2: %r' % log)
print('no such unit: refused; %d checks' % checks[0])
print('ALL OK')
