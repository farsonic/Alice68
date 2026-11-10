#!/usr/bin/env python3
"""Alice68-Wheel on a 68k emulator (machine68k), against a fake USB card whose wheel counters
move between Delay()s and a fake input.device that records every event written to it.
Cases: wheel up/down/left/right, the 4th button, Shift held, the NEWMOUSE option, a flick
clamped to 8 steps, a second start (stops the first), no card, and a start from Workbench.

    harness_wheel.py amiga/out/Alice68-Wheel
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
sys.dont_write_bytecode = True
import machine68k as M
R = M.Register
fails = []
SYS, DOS, EXP, INP, LOAD, HEAP, ARGS, STACK, BOARD = 0x8000, 0xA000, 0xB000, 0xC000, 0x20000, 0x200000, 0x3F000, 0x80000, 0x300000
PROG = open(sys.argv[1], 'rb').read()

def run(args=b'', board=True, other_port=False, wb=False, script=None):
    """script: list of (wheel_y, wheel_x, mouse_btn, qualifier) applied at each Delay();
    at the Delay after the last one, SetSignal reports Ctrl-C"""
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'ev': [], 'delays': 0, 'qual': 0,
          'signal': None, 'ports': [], 'replied': [], 'pri': [], 'opened': 0, 'closed': 0}
    script = list(script or [])
    total = len(script)
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
    me = alloc(232)                                          # our process
    mem.w32(me + 172, 0 if wb else 0x1234)                   # pr_CLI
    wbmsg = alloc(40)
    mem.w_block(BOARD, bytes(0x10000))
    if board:
        mem.w32(BOARD, 0x45415550)
        mem.w32(BOARD + 44, 100 & 0xffffffff); mem.w32(BOARD + 48, (-3) & 0xffffffff)   # counters already moved
    cd = alloc(48); mem.w32(cd + 32, BOARD); mem.w32(cd + 36, 0x10000)
    lvo(EXP, -72, lambda o, p: setd0(cd if (d(R.A0) == 0 and board) else 0))
    lvo(SYS, -552, lambda o, p: setd0({'dos.library': DOS, 'expansion.library': EXP}.get(mem.r_cstr(d(R.A1)), 0)))
    lvo(SYS, -414, lambda o, p: None)
    for off in (-132, -138): lvo(SYS, off, lambda o, p: None)
    otherp = alloc(34); mem.w32(otherp + 16, 0x4321)
    lvo(SYS, -390, lambda o, p: setd0(otherp if other_port and mem.r_cstr(d(R.A1)) == 'Alice68-Wheel' else 0))
    def signal(o, p): st['signal'] = (d(R.A1), d(R.D0))
    lvo(SYS, -324, signal)
    lvo(SYS, -294, lambda o, p: setd0(me))
    lvo(SYS, -300, lambda o, p: (st['pri'].append(d(R.D0)), setd0(0)))
    lvo(SYS, -666, lambda o, p: setd0(alloc(34)))
    lvo(SYS, -672, lambda o, p: None)
    lvo(SYS, -654, lambda o, p: setd0(alloc(d(R.D0))))
    lvo(SYS, -660, lambda o, p: None)
    def opendev(o, p):
        if mem.r_cstr(d(R.A0)) == 'input.device': mem.w32(d(R.A1) + 20, INP); st['opened'] += 1; setd0(0)
        else: setd0(0xff)
    lvo(SYS, -444, opendev)
    lvo(SYS, -450, lambda o, p: st.__setitem__('closed', st['closed'] + 1))
    def doio(o, p):
        io = d(R.A1)
        if mem.r16(io + 28) == 11 and mem.r32(io + 36) == 22:
            ie = mem.r32(io + 40)
            st['ev'].append((mem.r8(ie + 4), mem.r16(ie + 6), mem.r16(ie + 8)))
        setd0(0)
    lvo(SYS, -456, doio)
    lvo(SYS, -354, lambda o, p: st['ports'].append(mem.r_cstr(mem.r32(d(R.A1) + 10))))
    lvo(SYS, -360, lambda o, p: st['ports'].append('-removed'))
    def setsignal(o, p): setd0(0x1000 if st['delays'] > total else 0)     # Ctrl-C one Delay after the script
    lvo(SYS, -306, setsignal)
    lvo(SYS, -384, lambda o, p: None)                        # WaitPort
    lvo(SYS, -372, lambda o, p: setd0(wbmsg if wb else 0))   # GetMsg
    lvo(SYS, -378, lambda o, p: st['replied'].append(d(R.A1)))
    lvo(INP, -42, lambda o, p: setd0(st['qual']))
    lvo(DOS, -60, lambda o, p: setd0(1))
    lvo(DOS, -48, lambda o, p: (st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')), setd0(d(R.D3))))
    def delay(o, p):
        st['delays'] += 1
        if script:
            y, x, b, q = script.pop(0)
            mem.w32(BOARD + 44, y & 0xffffffff); mem.w32(BOARD + 48, x & 0xffffffff); mem.w32(BOARD + 52, b); st['qual'] = q
    lvo(DOS, -198, delay)
    w = lambda o: struct.unpack('>I', PROG[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(PROG[32:32 + n]); pos = 32 + n
    if w(pos) == 0x3ec:
        for i in range(w(pos + 4)):
            r = w(pos + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    mem.w_block(ARGS, args + b'\n\0'); mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(args) + 1); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(4000):
        if st['done']: break
        cpu.execute(100000)
    st['rc'] = cpu.r_reg(R.D0)
    st['text'] = ''.join(st['out'])
    st['wbmsg'] = wbmsg
    return st

def check(name, cond, st):
    if not cond: fails.append('%s (out: %r, events: %r)' % (name, st['text'], st['ev']))

UP, DOWN, LEFT, RIGHT, FOURTH, RAW, NEW = 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 1, 0x16
def keys(*codes, qual=0, cls=RAW): return [(cls, c, qual) for c in codes]

# wheel up 2, down 1, right 1, the 4th button pressed and released with Shift held, a flick of 20
s = run(script=[(102, -3, 0, 0), (101, -3, 0, 0), (101, -2, 0, 0), (101, -2, 8, 1), (101, -2, 0, 1), (121, -2, 0, 0)])
check('runs', s['done'] and not s['exc'] and s['rc'] == 0 and 'running' in s['text'], s)
want = (keys(UP, UP | 0x80, UP, UP | 0x80) + keys(DOWN, DOWN | 0x80) + keys(RIGHT, RIGHT | 0x80)
        + keys(FOURTH, qual=1) + keys(FOURTH | 0x80, qual=1) + keys(*([UP, UP | 0x80] * 8)))
check('events', s['ev'] == want, s)
check('public port, removed at the end', s['ports'] == ['Alice68-Wheel', '-removed'], s)
check('priority 5, then back', s['pri'][:1] == [5] and len(s['pri']) == 2, s)
check('input.device closed', s['opened'] == 1 and s['closed'] == 1, s)

# NEWMOUSE: each one also as IECLASS_NEWMOUSE, before the raw key
s = run(args=b'NEWMOUSE', script=[(99, -3, 0, 0)])
check('newmouse', s['ev'] == [(NEW, DOWN, 0), (RAW, DOWN, 0), (NEW, DOWN | 0x80, 0), (RAW, DOWN | 0x80, 0)], s)
check('newmouse says so', 'NewMouse' in s['text'], s)

# a second start stops the first (Ctrl-C to its port's task) and does nothing else
s = run(other_port=True)
check('second start', s['rc'] == 0 and s['signal'] == (0x4321, 0x1000) and 'stopped' in s['text'] and not s['ev'] and s['opened'] == 0, s)

# no USB card: says why, fails
s = run(board=False)
check('no card', s['rc'] == 10 and 'no Alice68 USB card' in s['text'] and s['opened'] == 0, s)

# from Workbench: the startup message taken and replied, nothing written to a console
s = run(wb=True, script=[(101, -3, 0, 0)])
check('workbench', s['rc'] == 0 and s['replied'] == [s['wbmsg']] and s['text'] == '' and s['ev'] == keys(UP, UP | 0x80), s)

if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('Alice68-Wheel: wheels, 4th button, qualifiers, NEWMOUSE, clamp, second start, no card, Workbench')
print('ALL OK')
