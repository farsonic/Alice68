#!/usr/bin/env python3
"""Alice68-JoyTest on a 68k emulator (machine68k), with 16 MB of RAM so that JOY0DAT/JOY1DAT/
POTGOR ($DFF00A/$DFF00C/$DFF016) and CIA-A's PRA ($BFE001) are memory the test sets, and a fake
potgo.resource: ONCE decodes the ports as games do (the quadrature directions, fire on CIA-A,
the second and third buttons on POTGOR pins 9 and 5); the loop prints a line on every change and
none without one, stops on Ctrl-C, takes the joystick port's POTGO bits as inputs and gives them
back.

    harness_joytest.py amiga/out/Alice68-JoyTest
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
LOAD, SYS, DOS, POT, ARGS, STACK = 0x20000, 0x8000, 0xA000, 0xC000, 0x3F000, 0x80000
JOY0, JOY1, POTGOR, PRA = 0xDFF00A, 0xDFF00C, 0xDFF016, 0xBFE001

def joy(up=0, down=0, left=0, right=0):
    return (left << 9) | ((left ^ up) << 8) | (right << 1) | (right ^ down)

def run(args, states, brk_after=None):
    mach = M.Machine(M.CPUType.M68020, 16384)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'out': [], 'delays': 0, 'pot': []}
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
    lvo(SYS, -552, lambda o, p: setd0({'dos.library': DOS}.get(mem.r_cstr(d(R.A1)), 0)))
    lvo(SYS, -498, lambda o, p: setd0(POT if mem.r_cstr(d(R.A1)) == 'potgo.resource' else 0))
    lvo(SYS, -414, lambda o, p: None)
    lvo(SYS, -306, lambda o, p: setd0(1 << 12 if brk_after is not None and st['delays'] >= brk_after else 0))
    lvo(DOS, -60, lambda o, p: setd0(1))
    def write(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); setd0(d(R.D3))
    lvo(DOS, -48, write)
    def setstate(i):
        s = states[min(i, len(states) - 1)]
        mem.w16(JOY1, s.get('j1', 0)); mem.w16(JOY0, s.get('j0', 0))
        mem.w16(POTGOR, s.get('pg', 0xFF00)); mem.w8(PRA, s.get('pra', 0xFC))
    def delay(o, p):
        st['delays'] += 1
        setstate(st['delays'])
    lvo(DOS, -198, delay)
    lvo(POT, -6, lambda o, p: (st['pot'].append(('alloc', d(R.D0))), setd0(d(R.D0))))
    lvo(POT, -12, lambda o, p: st['pot'].append(('free', d(R.D0))))
    lvo(POT, -18, lambda o, p: st['pot'].append(('write', d(R.D0), d(R.D1))))
    setstate(0)
    h = open(sys.argv[1], 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    a = args.encode() + b'\n'
    mem.w_block(ARGS, a + b'\0'); mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(a)); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(400):
        if st['done']: break
        cpu.execute(20000)
    if st['exc']: fails.append('%r: CPU exception %d at $%x' % (args, *st['exc']))
    if not st['done']: fails.append('%r: did not return' % args)
    return ''.join(st['out']), st

def ports(line):
    if 'Mouse port:' not in line: return line.split('Joystick port:')[1].split('(')[0].split(), None
    a, b = line.split('Mouse port:')
    return a.split('Joystick port:')[1].split(), b.split('(')[0].split()

# ONCE: up+left, fire (CIA-A bit 7 low), fire 2 (POTGOR bit 14 low), fire 3 (bit 12 low); the mouse port: right, its fire
text, st = run('ONCE MOUSEPORT', [dict(j1=joy(up=1, left=1), pra=0x3C, pg=0xFF00 & ~0x4000 & ~0x1000, j0=joy(right=1))])
lines = [l for l in text.splitlines() if 'Joystick port:' in l]
if len(lines) != 1: fails.append('ONCE: %d lines' % len(lines))
else:
    j, m = ports(lines[0])
    if j != ['UP', '-', 'LEFT', '-', 'FIRE', 'FIRE2', 'FIRE3']: fails.append('ONCE joystick port: %r' % j)
    if m != ['-', '-', '-', 'RIGHT', 'FIRE', '-']: fails.append('ONCE mouse port: %r' % m)
if ('alloc', 0xF000) not in st['pot'] or ('write', 0, 0xF000) not in st['pot'] or ('free', 0xF000) not in st['pot']:
    fails.append('potgo.resource: %r' % st['pot'])
# each direction alone, then down+right (both quadrature bits)
for name, kw in (('UP', dict(up=1)), ('DOWN', dict(down=1)), ('LEFT', dict(left=1)), ('RIGHT', dict(right=1))):
    text, st = run('ONCE', [dict(j1=joy(**kw))])
    j, m = ports([l for l in text.splitlines() if 'Joystick port:' in l][0])
    if [x for x in j if x != '-'] != [name]: fails.append('%s alone: %r' % (name, j))
text, st = run('ONCE', [dict(j1=joy(down=1, right=1))])
j, m = ports([l for l in text.splitlines() if 'Joystick port:' in l][0])
if [x for x in j if x != '-'] != ['DOWN', 'RIGHT']: fails.append('down+right: %r' % j)
# without MOUSEPORT a mouse moving in the mouse port prints nothing
text, st = run('', [dict(), dict(j0=0x0102), dict(j0=0x0305), dict(j0=0x0708), dict()], brk_after=4)
lines = [l for l in text.splitlines() if 'Joystick port:' in l]
if len(lines) != 1 or 'Mouse port' in text: fails.append('a moving mouse: %d lines %r' % (len(lines), lines))
# the loop: a line per change only, Ctrl-C ends it
seq = [dict(), dict(), dict(j1=joy(up=1)), dict(j1=joy(up=1)), dict(j1=joy(up=1), pra=0x7C), dict(), dict(), dict()]
text, st = run('', seq, brk_after=7)
lines = [l for l in text.splitlines() if 'Joystick port:' in l]
if len(lines) != 4 or '*** Break' not in text: fails.append('loop: %d lines, break %s: %r' % (len(lines), '*** Break' in text, lines))
if ('free', 0xF000) not in st['pot']: fails.append('loop: POTGO bits not given back')
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('directions, fire, fire 2 and 3 as games read them; the mouse port only with MOUSEPORT (a moving mouse prints nothing); a line per change; Ctrl-C; POTGO bits taken and given back')
print('ALL OK')
