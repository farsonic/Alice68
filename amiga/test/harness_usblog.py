#!/usr/bin/env python3
"""Alice68 LOG (was Alice68-USBLog) on a 68k emulator (machine68k) against a fake USB pass-through board
with a long kernel log: by default only the last 40 lines; LINES=n; ALL; MATCH=text (any
case, with the brackets of "[BT]"); Ctrl-C stops it in the middle of a long log; a
word with an 'f' in it is no FOLLOW (it once was, and the tool never ended).

    harness_usblog.py amiga/out/Alice68   (its LOG subcommand)
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
LOAD, SYS, DOS, EXP, ARGS, STACK = 0x20000, 0x8000, 0xA000, 0xE000, 0x6F000, 0x80000

def run(args, brk_after=None):
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'out': [], 'writes': 0, 'heap': 0x200000}
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
    lvo(SYS, -552, lambda o, p: setd0({'dos.library': DOS, 'expansion.library': EXP}.get(mem.r_cstr(d(R.A1)), 0)))
    for off in (-414, -132, -138, -450): lvo(SYS, off, lambda o, p: None)
    lvo(SYS, -276, lambda o, p: setd0(0))                      # alice68-usb.device not loaded
    lvo(SYS, -444, lambda o, p: setd0(0xffffffff))
    def setsignal(o, p):                                       # Ctrl-C once enough has been written
        setd0(1 << 12 if brk_after is not None and st['writes'] >= brk_after else 0)
    lvo(SYS, -306, setsignal)
    lvo(DOS, -60, lambda o, p: setd0(1))
    def write(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); st['writes'] += 1; setd0(d(R.D3))
    lvo(DOS, -48, write)
    lvo(DOS, -198, lambda o, p: None)
    # the board: a kernel log of 300 lines, every 10th a Bluetooth one
    BOARD = 0x100000
    mem.w32(BOARD, 0x45415550)
    log = ''.join(('[BT] line %d from the chip\n' % i) if i % 10 == 5 else ('[FRF-XHCI] line %d\n' % i) for i in range(300)).encode()
    mem.w_block(BOARD + 0x8000, log); mem.w32(BOARD + 24, len(log))
    alog = b'[alice68-usb] opened\n'; mem.w_block(BOARD + 0x1000, alog); mem.w32(BOARD + 28, len(alog))
    CD = 0x1F000
    mem.w32(CD + 32, BOARD); mem.w32(CD + 36, 0x10000)
    mem.w8(CD + 16 + 1, 0x6a); mem.w8(CD + 16 + 4, 0x6d); mem.w8(CD + 16 + 5, 0x73)
    def findcd(o, p):
        old, man, prod = d(R.A0), d(R.D0), d(R.D1)
        setd0(CD if old == 0 else 0)
    lvo(EXP, -72, findcd)
    h = open(sys.argv[1], 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    a = ('LOG ' + args).encode() + b'\n'          # C:Alice68's LOG subcommand
    mem.w_block(ARGS, a + b'\0'); mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(a)); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(3000):
        if st['done']: break
        cpu.execute(100000)
    if st['exc']: fails.append('%r: CPU exception %d at $%x' % (args, *st['exc']))
    if not st['done']: fails.append('%r: did not return (still printing, or following)' % args)
    text = ''.join(st['out'])
    klog = text.split("--- the kernel's log ---\n", 1)[1].split('\n--- alice68-usb.device ---', 1)[0] if "--- the kernel's log ---" in text else ''
    return text, [l for l in klog.splitlines() if l.startswith('[')]

text, lines = run('')
if len(lines) != 40 or lines[-1] != '[FRF-XHCI] line 299' or '260 earlier lines left out' not in text: fails.append('default: %d lines, last %r' % (len(lines), lines[-1:] ))
text, lines = run('MATCH=[bt]')
if len(lines) != 30 or not all(l.startswith('[BT]') for l in lines) or lines[0] != '[BT] line 5 from the chip': fails.append('MATCH=[bt]: %d %r' % (len(lines), lines[:2]))
text, lines = run('MATCH="from the"  LINES=5')
if [l.split()[2] for l in lines] != ['245', '255', '265', '275', '285', '295'][1:]: fails.append('MATCH + LINES=5: %r' % lines)
text, lines = run('ALL')
if len(lines) != 300: fails.append('ALL: %d lines' % len(lines))
text, lines = run('ALL', brk_after=30)
if '*** Break' not in text or len(lines) > 40: fails.append('Ctrl-C: %d lines, %s' % (len(lines), 'Break' in text))
text, lines = run('>RAM:file')                                 # an 'f' in a word is not FOLLOW
if 'following' in text: fails.append('a stray f started FOLLOW')
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('default last 40 lines; MATCH=[bt]; MATCH with LINES; ALL; Ctrl-C mid-log; no FOLLOW from a stray f')
print('ALL OK')
