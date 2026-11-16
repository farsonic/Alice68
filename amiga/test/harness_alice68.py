#!/usr/bin/env python3
"""C:Alice68's dispatcher on a 68k emulator (machine68k): every subcommand (any case, with
its arguments) reaches its tool's main with the rest of the line; no argument and HELP list
them all; an unknown word is named and returns 10; started from Workbench (no pr_CLI) it is
the wheel helper. Each tool's main is replaced by a trap that records its arguments (the
tools themselves have harnesses of their own: harness_status.py, harness_usblog.py, ...).

    harness_alice68.py amiga/out/Alice68
SPDX-License-Identifier: GPL-2.0-only"""
import struct, subprocess, sys
import machine68k as M
R = M.Register
fails = []
LOAD, SYS, DOS, ARGS, STACK, PROC = 0x20000, 0x8000, 0xA000, 0x6F000, 0x80000, 0x1E000
hunk = sys.argv[1]
nm = subprocess.run(['m68k-linux-gnu-nm', hunk + '.elf'], capture_output=True, text=True).stdout
mains = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if l.endswith('_main') and ' T ' in l and not l.endswith(' alice68_main')}
SUBS = {'STATUS': 'status_main', 'LOG': 'usblog_main', 'JOYTEST': 'joytest_main', 'WIFI': 'wifitool_main',
        'HDF': 'hdfinfo_main', 'NETCHECK': 'netcheck_main', 'SHOWCONFIG': 'showconfig_main',
        'MONITOR': 'monitor_main', 'WHEEL': 'wheel_main'}
for s, f in SUBS.items():
    if f not in mains: fails.append('no %s in the binary' % f)

def run(args, cli=True):
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'out': [], 'called': None, 'rc': None}
    def d(r): return cpu.r_reg(r)
    def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)
    def lvo(base, off, fn): mem.w16(base + off, 0xA000 | traps.setup(fn, auto_rts=True))
    def t_exit(op, pc): st['done'] = True; st['rc'] = d(R.D0); cpu.end()
    mem.w16(0x1000, 0xA000 | traps.setup(t_exit))
    def t_exc(op, pc):
        sp = cpu.r_reg(R.A7); st['exc'] = ((mem.r16(sp + 6) & 0xfff) // 4, mem.r32(sp + 2)); st['done'] = True; cpu.end()
    mem.w16(0x1010, 0xA000 | traps.setup(t_exc))
    for v in range(2, 64): mem.w32(v * 4, 0x1010)
    mem.w32(4, SYS)
    mem.w32(PROC + 172, (0x1E200 >> 2) if cli else 0)
    lvo(SYS, -294, lambda o, p: setd0(PROC if d(R.A1) == 0 else 0))
    lvo(SYS, -552, lambda o, p: setd0(DOS if mem.r_cstr(d(R.A1)) == 'dos.library' else 0))
    lvo(SYS, -414, lambda o, p: None)
    lvo(DOS, -60, lambda o, p: setd0(1))
    def write(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); setd0(d(R.D3))
    lvo(DOS, -48, write)
    h = open(hunk, 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    for name, off in mains.items():               # each tool's main: a trap that says who was called, with what
        def t_main(op, pc, name=name):
            sp = cpu.r_reg(R.A7)
            a, l = mem.r32(sp + 4), mem.r32(sp + 8)
            st['called'] = (name, bytes(mem.r_block(a, l)).decode('latin1') if l else '')
            setd0(7)
        mem.w16(LOAD + off, 0xA000 | traps.setup(t_main, auto_rts=True))
    a = args.encode() + b'\n'
    mem.w_block(ARGS, a + b'\0'); mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(a)); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(200):
        if st['done']: break
        cpu.execute(20000)
    if st['exc']: fails.append('%r: CPU exception %d at $%x' % (args, *st['exc']))
    if not st['done']: fails.append('%r: did not return' % args)
    return ''.join(st['out']), st

for sub, fn in SUBS.items():
    for spelt in (sub, sub.lower(), sub.capitalize()):
        text, st = run('%s  ONE "two words" 3' % spelt)
        if not st['called'] or st['called'][0] != fn: fails.append('%s: called %r' % (spelt, st['called'])); continue
        if st['called'][1] != 'ONE "two words" 3\n': fails.append('%s: arguments %r' % (spelt, st['called'][1]))
        if st['rc'] != 7: fails.append('%s: return code %r (the tool\'s is 7)' % (spelt, st['rc']))
    text, st = run(sub)                         # no arguments: the tool gets the end of the line
    if not st['called'] or st['called'][1].strip(): fails.append('%s alone: %r' % (sub, st['called']))
for args in ('', 'HELP', 'help', '?'):
    text, st = run(args)
    if st['called'] or st['rc'] != 0 or any(('Alice68 ' + s) not in text for s in SUBS) or 'Alice68Prefs' not in text:
        fails.append('%r: the list: rc %r, called %r, %r' % (args, st['rc'], st['called'], text[:120]))
text, st = run('USBLOG')
if st['called'] or st['rc'] != 10 or 'no subcommand "USBLOG"' not in text or 'Alice68 LOG' not in text:
    fails.append('unknown word: rc %r, %r' % (st['rc'], text[:100]))
text, st = run('STATUS', cli=False)            # from Workbench: the wheel helper, whatever the line
if not st['called'] or st['called'][0] != 'wheel_main' or st['called'][1]: fails.append('from Workbench: %r' % (st['called'],))
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('%d subcommands in any case with their arguments; no word, HELP and ? list them; an unknown one named (10); Workbench: the wheel' % len(SUBS))
print('ALL OK')
