#!/usr/bin/env python3
"""Alice68-Status on a 68k emulator (machine68k) against a fake devicetree.resource and a fake
status page (third_party/Emu68/src/alice/alice_status.h): it finds the page through /alice68
"status", prints every part of it (unknown values as "not measured"/"not counted", the mailbox
sensors only with their flag), waits while the page is being written (odd seq), says so when
there is no page, and LOOP stops on Ctrl-C.

    harness_status.py amiga/out/Alice68-Status
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
LOAD, SYS, DOS, DTR, ARGS, STACK = 0x20000, 0x8000, 0xA000, 0xC000, 0x3F000, 0x80000
PAGE = 0x100000
U = 0xffffffff

def page(mem, seq=10, flags=0, sensors=True):
    def w(o, v): mem.w32(PAGE + o, v & 0xffffffff)
    def s(o, t, n): mem.w_block(PAGE + o, t.encode()[:n - 1].ljust(n, b'\0'))
    for o in range(0, 0x1a0, 4): w(o, 0)
    w(0, 0x41363853); w(4, 1); w(8, 0x1a0); w(12, seq); w(0x10, 3723000); w(0x14, 1000); w(0x18, flags)
    s(0x20, 'Alice68 0.6.0 (15.11.2026) git:abcdef0, based on Emu68 1.1.0-beta.1', 96)
    for i, (role, busy, name) in enumerate([(1, U, '68k JIT'), (2, 1234, 'USB, network'), (3, U, 'display'), (4, U, 'chipset, audio')]):
        w(0x80 + 32 * i, role); w(0x84 + 32 * i, busy); s(0x88 + 32 * i, name, 24)
    w(0x100, 52345)
    if sensors: w(0x104, 1800000000); w(0x108, 850000); w(0x10c, 0x50000); w(0x110, 3700000)
    else: w(0x104, U); w(0x108, U); w(0x10c, U)
    w(0x120, 64 << 20); w(0x124, 40 << 20); w(0x128, 1234); w(0x12c, 56789); w(0x130, U); w(0x134, U)
    w(0x140, 1); w(0x144, 1); w(0x148, 100); w(0x14c, 50); w(0x150, 30 << 20); w(0x154, 2048); w(0x158, 0); w(0x15c, 0)
    w(0x160, 0)
    w(0x180, U); w(0x184, U); w(0x188, U); w(0x18c, U)

def run(args, have_dt=True, flags=0, sensors=True, odd_reads=0, brk_after=None):
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'out': [], 'delays': 0, 'seqreads': 0}
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
    lvo(SYS, -498, lambda o, p: setd0(DTR if have_dt and mem.r_cstr(d(R.A1)) == 'devicetree.resource' else 0))
    for off in (-414, -132, -138): lvo(SYS, off, lambda o, p: None)
    lvo(SYS, -306, lambda o, p: setd0(1 << 12 if brk_after is not None and st['delays'] >= brk_after else 0))
    lvo(DOS, -60, lambda o, p: setd0(1))
    def write(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); setd0(d(R.D3))
    lvo(DOS, -48, write)
    def delay(o, p):
        st['delays'] += 1
        if st.get('odd'):                          # the kernel's write ends while the reader waits
            st['odd'] -= 1
            if not st['odd']: mem.w32(PAGE + 12, 12)
    lvo(DOS, -198, delay)
    # devicetree.resource: one key, two properties
    KEY, PS, PV, VS, VV = 0x30000, 0x30010, 0x30020, 0x30030, 0x30040
    mem.w32(PV, PAGE); mem.w32(VV, 1)
    lvo(DTR, -6, lambda o, p: setd0(KEY if mem.r_cstr(d(R.A0)) == '/alice68' else 0))
    lvo(DTR, -12, lambda o, p: None)
    lvo(DTR, -24, lambda o, p: setd0({'status': PS, 'status-version': VS}.get(mem.r_cstr(d(R.A1)), 0) if d(R.A0) == KEY else 0))
    lvo(DTR, -48, lambda o, p: setd0({PS: PV, VS: VV}.get(d(R.A0), 0)))
    page(mem, flags=flags, sensors=sensors)
    if odd_reads:                                  # the kernel is writing: seq odd for a few reads
        mem.w32(PAGE + 12, 11)
        st['odd'] = odd_reads
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

text, st = run('')
for want in ('Status page at $00100000, version 1', 'Kernel    Alice68 0.6.0 (15.11.2026) git:abcdef0', 'Uptime    1:02:03',
             'Core 0    68k JIT         busy: not measured', 'Core 1    USB, network    busy: 12.3 %',
             'Core 3    chipset, audio  busy: not measured', 'SoC       52.3 C (clock, voltage: status.sensors=mbox in cmdline.txt)',
             'JIT       cache 64 MB, free 40 MB, 1234 blocks, 56789 translated since boot',
             'Unit 0    WiFi, link up, in 30 MB (100 frames), out 2048 bytes (50 frames)', 'Unit 1    (none)', 'USB, SD   not counted'):
    if want not in text: fails.append('once: %r missing' % want)
if st['delays']: fails.append('once: it waited')
text, st = run('', flags=1)
if 'ARM 1800 MHz, core 0.850 V, throttled $00050000' not in text: fails.append('mbox sensors: %r' % [l for l in text.splitlines() if l.startswith('SoC')])
text, st = run('', flags=1, sensors=False)
if 'SoC       52.3 C, clock and voltage not read yet' not in text: fails.append('mbox sensors not read yet: %r' % [l for l in text.splitlines() if l.startswith('SoC')])
text, st = run('', odd_reads=3)
if 'Kernel    Alice68' not in text or st['delays'] != 3: fails.append('odd seq (%d waits)' % st['delays'] + ': the page was not read after the write ended: %r' % text[:200])
text, st = run('', have_dt=False)
if 'no status page' not in text: fails.append('no devicetree.resource: %r' % text)
text, st = run('LOOP', brk_after=12)
if text.count('Kernel    ') < 2 or '*** Break' not in text: fails.append('LOOP: %d pages, break %s' % (text.count('Kernel    '), '*** Break' in text))
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('finds the page, prints all of it, unknowns as such, mailbox sensors with their flag; odd seq waited out; no page; LOOP and Ctrl-C')
print('ALL OK')
