#!/usr/bin/env python3
"""Alice68-Monitor DUMP on a 68k emulator (machine68k), against a fake
pass-through board with log rings, the WiFi driver's state, and a fake exec
task list whose stack holds a return address into the "driver".

    harness_monitor.py amiga/out/Alice68-Monitor
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
mach = M.Machine(M.CPUType.M68020, 4096)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYS, DOS, EXP, LOAD, HEAP, ARGS, STACK, BOARD = 0x8000, 0xA000, 0xB000, 0x20000, 0x200000, 0x3F000, 0x80000, 0x300000
st = {'done': False, 'exc': None, 'heap': HEAP, 'out': []}
def d(r): return cpu.r_reg(r)
def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)
def lvo(base, off, fn): mem.w16(base + off, 0xA000 | traps.setup(fn, auto_rts=True))
def t_exit(op, pc): st['done'] = True; cpu.end()
mem.w16(0x1000, 0xA000 | traps.setup(t_exit))
def t_exc(op, pc):
    sp = cpu.r_reg(R.A7); st['exc'] = ((mem.r16(sp + 6) & 0xfff) // 4, mem.r32(sp + 2)); st['done'] = True; cpu.end()
mem.w16(0x1010, 0xA000 | traps.setup(t_exc))
for v in range(2, 64): mem.w32(v * 4, 0x1010)
mem.w32(4, SYS)
def alloc(n):
    a = (st['heap'] + 7) & ~7; st['heap'] = a + n; mem.w_block(a, bytes(n)); return a
def cstr(s): a = alloc(len(s) + 1); mem.w_block(a, s.encode() + b'\0'); return a
def newlist(a): mem.w32(a, a + 4); mem.w32(a + 4, 0); mem.w32(a + 8, a)
def addtail(l, n):
    pred = mem.r32(l + 8); mem.w32(n, l + 4); mem.w32(n + 4, pred); mem.w32(pred, n); mem.w32(l + 8, n)
for off in (322, 350, 406, 420): newlist(SYS + off)
# memory list: one header covering 0..4 MB
mh = alloc(32); mem.w32(mh + 20, 0x400); mem.w32(mh + 24, 0x400000); addtail(SYS + 322, mh)
mem.w32(SYS + 300, 0)                                      # no ResModules
# the WiFi "driver": a device node with a seglist; its code has a JSR at code+0x100
seg = alloc(0x2000); mem.w32(seg, 0x2000); segb = seg + 4    # size long, then the BPTR next
code = segb + 4
mem.w16(code + 0xfa, 0x4eb9); mem.w32(code + 0xfc, 0x12345678)    # jsr abs.l: return address code+0x100
dev = alloc(64); mem.w32(dev + 10, cstr('alice68wifi.device')); mem.w32(dev + 36, segb >> 2); addtail(SYS + 350, dev)
# tasks: ourselves running, the driver's process waiting with that return address on its stack
me = alloc(92); mem.w32(me + 10, cstr('Shell Process')); mem.w8(me + 15, 2); mem.w8(me + 8, 1)
mem.w32(SYS + 276, me)
wt = alloc(232); mem.w32(wt + 10, cstr('alice68wifi')); mem.w8(wt + 8, 1); mem.w8(wt + 15, 4); mem.w8(wt + 9, 0)
mem.w32(wt + 22, 0x100); stk = alloc(512); mem.w32(stk + 40, code + 0x100); mem.w32(wt + 54, stk); mem.w32(wt + 62, stk + 512)
addtail(SYS + 420, wt)
# the board
mem.w_block(BOARD, bytes(0x10000))
mem.w32(BOARD, 0x45415550)
klog = b'[FRF-XHCI] hello from the kernel\n'; mem.w_block(BOARD + 0x8000, klog); mem.w32(BOARD + 24, len(klog))
wlog = b'[    1.234] step 1: reading the firmware files\n'; mem.w_block(BOARD + 0x4000, wlog); mem.w32(BOARD + 36, len(wlog))
ws = BOARD + 0xC00; mem.w32(ws, 0x57535441); mem.w32(ws + 4, 77); mem.w32(ws + 16, wt); mem.w32(ws + 20, (1 << 16) | 11)
mem.w_block(ws + 32, b'DosRead DEVS:Firmware/cyfmac43455-sdio.bin 32768 bytes at 65536\0')
cd = alloc(48); mem.w8(cd + 16, 0xc1); mem.w32(cd + 32, BOARD); mem.w32(cd + 36, 0x10000)
def findcd(o, p):
    old, man, prod = d(R.A0), d(R.D0), d(R.D1)
    setd0(cd if old == 0 else 0)
lvo(EXP, -72, findcd)
lvo(SYS, -552, lambda o, p: setd0({'dos.library': DOS, 'expansion.library': EXP}.get(mem.r_cstr(d(R.A1)), 0)))
lvo(SYS, -414, lambda o, p: None)
for off in (-120, -126, -132, -138): lvo(SYS, off, lambda o, p: None)
def findname(o, p):
    l, n = d(R.A0), mem.r_cstr(d(R.A1)); x = mem.r32(l)
    while mem.r32(x):
        if mem.r32(x + 10) and mem.r_cstr(mem.r32(x + 10)) == n: setd0(x); return
        x = mem.r32(x)
    setd0(0)
lvo(SYS, -276, findname)
lvo(DOS, -60, lambda o, p: setd0(1))
lvo(DOS, -48, lambda o, p: (st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')), setd0(d(R.D3))))
h = open(sys.argv[1], 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
n = w(28) * 4; codeh = bytearray(h[32:32 + n]); p = 32 + n
if w(p) == 0x3ec:
    for i in range(w(p + 4)):
        r = w(p + 12 + 4 * i); v = struct.unpack('>I', codeh[r:r + 4])[0]; codeh[r:r + 4] = struct.pack('>I', v + LOAD)
mem.w_block(LOAD, bytes(codeh))
mem.w_block(ARGS, b'DUMP\n\0'); mem.w32(STACK, 0x1000)
cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, 5); cpu.w_pc(LOAD); cpu.w_sr(0)
for _ in range(4000):
    if st['done']: break
    cpu.execute(100000)
out = ''.join(st['out'])
if st['exc']: fails.append('CPU exception %d at $%x' % st['exc'])
if not st['done']: fails.append('did not return')
for want in ['alice68wifi 1.11: DosRead DEVS:Firmware/cyfmac43455-sdio.bin 32768 bytes at 65536', 'beat 77',
             '*alice68wifi', ' W w 00000100', 'wifi+0100', 'hello from the kernel', 'step 1: reading the firmware files',
             'the monitor has not run since']:
    if want not in out: fails.append('missing: ' + want)
if fails:
    print(out); print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print(out)
print('ALL OK')
