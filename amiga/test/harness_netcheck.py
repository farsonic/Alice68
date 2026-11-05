#!/usr/bin/env python3
"""Run Alice68-NetCheck on a 68k emulator (machine68k / Musashi) against a fake
SANA-II genet.device, in the situations it has to explain, and check its text.

    harness_netcheck.py amiga/out/Alice68-NetCheck
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M

R = M.Register
fails = []

def run(hunk, args, dev):
    """dev: dict(open_by_name, open_by_path, version, revision, irq, hwrx, hwtx, bps, ctrl_c_after)"""
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    SYSBASE, DOSBASE, DEVBASE, LOAD, HEAP, ARGS, STACK = 0x8000, 0xA000, 0xC000, 0x20000, 0x100000, 0x3F000, 0x80000
    st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'rounds': 0, 'opened': None}
    def d(r): return cpu.r_reg(r)
    def setd0(v): cpu.w_reg(R.D0, v & 0xffffffff)
    def lvo(base, off, fn): mem.w16(base + off, 0xA000 | traps.setup(fn, auto_rts=True))
    def t_exit(op, pc): st['done'] = True; cpu.end()
    mem.w16(0x1000, 0xA000 | traps.setup(t_exit))
    def t_exc(op, pc):
        sp = cpu.r_reg(R.A7); st['exc'] = ((mem.r16(sp + 6) & 0xfff) // 4, mem.r32(sp + 2)); st['done'] = True; cpu.end()
    mem.w16(0x1010, 0xA000 | traps.setup(t_exc))
    for v in range(2, 64): mem.w32(v * 4, 0x1010)
    mem.w32(4, SYSBASE)
    def alloc(n):
        a = (st['heap'] + 7) & ~7; st['heap'] = a + n; mem.w_block(a, bytes(n)); return a
    def cstr(s):
        a = alloc(len(s) + 1); mem.w_block(a, s.encode() + b'\0'); return a
    lvo(SYSBASE, -552, lambda o, p: setd0(DOSBASE if mem.r_cstr(d(R.A1)) == 'dos.library' else 0))
    lvo(SYSBASE, -414, lambda o, p: None)
    lvo(SYSBASE, -198, lambda o, p: setd0(alloc(d(R.D0))))
    lvo(SYSBASE, -210, lambda o, p: None)
    lvo(SYSBASE, -666, lambda o, p: setd0(alloc(34)))
    lvo(SYSBASE, -672, lambda o, p: None)
    lvo(SYSBASE, -654, lambda o, p: setd0(alloc(d(R.D0))))
    lvo(SYSBASE, -660, lambda o, p: None)
    def setsignal(o, p):
        st['rounds'] += 1
        setd0(0x1000 if dev.get('ctrl_c_after') and st['rounds'] >= dev['ctrl_c_after'] else 0)
    lvo(SYSBASE, -306, setsignal)
    lvo(SYSBASE, -132, lambda o, p: None)
    lvo(SYSBASE, -138, lambda o, p: None)
    lvo(SYSBASE, -276, lambda o, p: setd0(DEVBASE if dev.get('loaded') and mem.r_cstr(d(R.A1)) == 'genet.device' else 0))
    st['configured'] = dev.get('configured', False)
    st['paths'] = []
    lvo(DOSBASE, -60, lambda o, p: setd0(1))
    lvo(DOSBASE, -48, lambda o, p: (st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')), setd0(d(R.D3))))
    lvo(DOSBASE, -198, lambda o, p: None)
    # the fake device: a Library with a name, version and id string
    mem.w32(DEVBASE + 10, cstr('genet.device')); mem.w16(DEVBASE + 20, dev['version']); mem.w16(DEVBASE + 22, dev['revision'])
    mem.w32(DEVBASE + 24, cstr('genet.device %d.%d (01.11.2026)\r\n' % (dev['version'], dev['revision'])))
    names = {k: cstr(k) for k in ('rx_overruns', 'irq0_count', 'irq0_rx_count', 'hw_rx_pkts', 'hw_tx_pkts')}
    def opendev(o, p):
        name, io = mem.r_cstr(d(R.A0)), d(R.A1)
        st['paths'].append(name)
        ok = (name == 'genet.device' and dev['open_by_name']) or (name == 'DEVS:Networks/genet.device' and dev['open_by_path'])
        if ok:
            if mem.r32(io + 84) == 0: fails.append('opened without a buffer-management tag list')
            mem.w32(io + 20, DEVBASE); mem.w8(io + 31, 0); st['opened'] = name; setd0(0)
        else:
            mem.w8(io + 31, 0xff); setd0(0xffffffff)
    lvo(SYSBASE, -444, opendev)
    lvo(SYSBASE, -450, lambda o, p: st.__setitem__('closed', True))
    def doio(o, p):
        io = d(R.A1); cmd = mem.r16(io + 28); stat = mem.r32(io + 80); err = 0
        if cmd == 9:
            if mem.r32(stat) < 30: err = 3
            else:
                mem.w32(stat + 4, 30); mem.w16(stat + 16, 48); mem.w32(stat + 18, 1500); mem.w32(stat + 22, dev['bps']); mem.w32(stat + 26, 1)
        elif cmd == 10:                                   # SrcAddr: in use (0 until configured), DstAddr: factory
            for i, b in enumerate([0xdc, 0xa6, 0x32, 1, 2, 3]):
                mem.w8(io + 40 + i, b if st['configured'] else 0); mem.w8(io + 56 + i, b)
        elif cmd == 11:
            if st['configured']: err = 4
            else:
                st['configured'] = True; st['cfg_addr'] = bytes(mem.r_block(io + 40, 6))
        elif cmd == 22:
            for i, v in enumerate([dev['hwrx'], dev['hwtx'], 0, 0, 0, 2, 1]): mem.w32(stat + 4 * i, v)
            if dev.get('grow') and st['configured']:          # traffic once the interface is up
                dev['hwrx'] += 7; dev['hwtx'] += 1; dev['irq'] += (7 if dev['grow'] == 'irq' else 0)
        elif cmd == 21:
            mx = mem.r32(stat); recs = [('rx_overruns', 0), ('irq0_count', dev['irq']), ('irq0_rx_count', dev['irq']),
                                       ('hw_rx_pkts', dev['hwrx']), ('hw_tx_pkts', dev['hwtx'])][:mx]
            for i, (n, c) in enumerate(recs):
                a = stat + 8 + 12 * i; mem.w32(a, 0x1000 + i); mem.w32(a + 4, c); mem.w32(a + 8, names[n])
            mem.w32(stat + 4, len(recs))
        else:
            err = 8
        mem.w8(io + 31, err & 0xff); setd0(err)
    lvo(SYSBASE, -456, doio)
    # SendIO / CheckIO / WaitIO / AbortIO: a write completes at once, or (no link) never
    def sendio(o, p):
        io = d(R.A1); cmd = mem.r16(io + 28)
        if cmd != 3: fails.append('SendIO of command %d' % cmd); return
        st['writes'] = st.get('writes', 0) + 1
        bm = mem.r32(io + 84)
        tags = {}
        while bm and mem.r32(bm) != 0: tags[mem.r32(bm)] = mem.r32(bm + 4); bm += 8
        if tags.get(0x800B0002) is None or not (0x20000 <= tags[0x800B0002] < 0x30000):
            fails.append('test frame sent without an S2_CopyFromBuff function')
        if mem.r16(io + 36 - 4 + 4) is None: pass
        if bytes(mem.r_block(io + 56, 6)) != b'\xff' * 6 or mem.r32(io + 36) != 0x88b5: fails.append('test frame not a 0x88B5 broadcast')
        if dev.get('link', True):
            dev['hwtx'] += 1
            if dev.get('grow') != 'noirq': dev['irq'] += 1
            st['pending'] = None; mem.w8(io + 31, 0)
        else:
            st['pending'] = io
    lvo(SYSBASE, -462, sendio)
    lvo(SYSBASE, -468, lambda o, p: setd0(0 if st.get('pending') == d(R.A1) else d(R.A1)))
    def waitio(o, p):
        io = d(R.A1)
        if st.get('pending') == io: fails.append('WaitIO on a request that never completes')
        setd0(mem.r8(io + 31))
    lvo(SYSBASE, -474, waitio)
    def abortio(o, p):
        io = d(R.A1)
        if st.get('pending') == io: st['pending'] = None; mem.w8(io + 31, 0xfe); st['aborted'] = True
    lvo(SYSBASE, -480, abortio)
    # load the hunk
    h = open(hunk, 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    a = ARGS; mem.w_block(a, args.encode() + b'\n\0')
    sp = STACK; mem.w32(sp, 0x1000)
    cpu.w_reg(R.A7, sp); cpu.w_reg(R.A0, a); cpu.w_reg(R.D0, len(args) + 1); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(2000):
        if st['done']: break
        cpu.execute(100000)
    if st['exc']: fails.append('CPU exception %d at $%x' % st['exc'])
    if not st['done']: fails.append('did not return')
    return ''.join(st['out']), st

hunk = sys.argv[1]
base = dict(open_by_name=True, open_by_path=True, loaded=True, configured=True, version=3, revision=17,
            irq=0, hwrx=0, hwtx=0, bps=1000000000, ctrl_c_after=0)
unloaded = dict(base, open_by_name=False, loaded=False, configured=False)
cases = [
    ('loaded and configured by a stack, interrupts arrive', '', dict(base, irq=120, hwrx=340, hwtx=200),
     ['genet.device unit 0, version 3.17', 'line rate 1000 Mbit/s', 'factory dc:a6:32:01:02:03, in use dc:a6:32:01:02:03',
      'irq0_count', 'the driver works'], ['genet.device']),
    ('traffic but no interrupts (Alice68 bug)', '', dict(base, irq=0, hwrx=55, hwtx=3),
     ['NO interrupt reached AmigaOS', 'please report'], ['genet.device']),
    ('configured, no traffic', '', dict(base), ['sent and received nothing: no link'], ['genet.device']),
    ('loaded by a stack that holds it exclusively: no second copy', '', dict(base, open_by_name=False),
     ['refused a second', 'opened it for itself alone'], ['genet.device']),
    ('not loaded: from DEVS:Networks/, not configured', '', dict(unloaded),
     ['DEVS:Networks/genet.device unit 0 (not loaded before', 'not configured: no TCP/IP stack', 'Alice68-NetCheck TEST'],
     ['genet.device', 'DEVS:Networks/genet.device']),
    ('missing', '', dict(unloaded, open_by_path=False), ['cannot open it', 'version 3.x'], None),
    ('netdev 4.x installed', '', dict(base, version=4, revision=4), ['is the netdev driver', 'use the SANA-II driver'], None),
    ('WATCH until Ctrl-C', 'WATCH', dict(base, irq=7, hwrx=9, ctrl_c_after=3), ['Watching', 'irq0 7', '***Break'], None),
    ('TEST: brings it up, interrupts arrive', 'TEST', dict(unloaded, grow='irq'),
     ['bringing the interface up with the factory address... online', 'Watching for 5 seconds', 'their interrupts reached AmigaOS'], None),
    ('TEST: traffic but no interrupts', 'TEST', dict(unloaded, grow='noirq'),
     ['online', 'NO interrupt reached AmigaOS'], None),
    ('TEST on a configured interface leaves it alone', 'TEST', dict(base, irq=3, hwrx=4),
     ['already configured by a TCP/IP stack', 'sending 3 broadcast test frames... sent'], None),
    ('TEST, quiet network: the test frames bring interrupts', 'TEST', dict(unloaded),
     ['online', 'sending 3 broadcast test frames... sent', 'their interrupts reached AmigaOS'], None),
    ('TEST, quiet network: frames go out, no interrupt', 'TEST', dict(unloaded, grow='noirq'),
     ['sent', 'NO interrupt reached AmigaOS'], None),
    ('TEST, no link: the driver holds the frame', 'TEST', dict(unloaded, link=False),
     ['no answer in 4 s', 'no link'], None),
]
for name, args, dev, want, paths in cases:
    out, st = run(hunk, args, dev)
    missing = [w for w in want if w not in out]
    if missing: fails.append('%s: missing %s\n----\n%s----' % (name, missing, out))
    elif paths is not None and st['paths'] != paths: fails.append('%s: opened %s, want %s' % (name, st['paths'], paths))
    elif 'TEST' in args and 'online' in out and st.get('cfg_addr') != bytes([0xdc, 0xa6, 0x32, 1, 2, 3]):
        fails.append('%s: configured with %s, want the factory address' % (name, st.get('cfg_addr')))
    elif st.get('opened') and not st.get('closed'): fails.append('%s: device not closed' % name)
    else: print('ok  %s' % name)
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
# the copy routine the driver calls: a0 = to, a1 = from, d0 = bytes -> TRUE
import subprocess
nm = subprocess.run(['m68k-linux-gnu-nm', hunk + '.elf'], capture_output=True, text=True).stdout
addr = [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' nc_copybuff')]
if not addr: fails.append('no nc_copybuff symbol')
else:
    mach = M.Machine(M.CPUType.M68020, 1024); cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    h = open(hunk, 'rb').read(); n = struct.unpack('>I', h[28:32])[0] * 4
    mem.w_block(0x20000, h[32:32 + n])
    done = {'f': False}
    def t_exit(op, pc): done['f'] = True; cpu.end()
    mem.w16(0x1000, 0xA000 | traps.setup(t_exit))
    mem.w_block(0x5000, b'Alice68 copy'); mem.w_block(0x6000, bytes(16))
    mem.w32(0x8000, 0x1000); cpu.w_reg(R.A7, 0x8000)
    cpu.w_reg(R.A0, 0x6000); cpu.w_reg(R.A1, 0x5000); cpu.w_reg(R.D0, 12); cpu.w_pc(0x20000 + addr[0]); cpu.w_sr(0)
    for _ in range(100):
        if done['f']: break
        cpu.execute(1000)
    if bytes(mem.r_block(0x6000, 13)) != b'Alice68 copy\0' or cpu.r_reg(R.D0) != 1: fails.append('nc_copybuff copies wrong')
    else: print('ok  nc_copybuff copies and returns TRUE')
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('--- sample: TEST on a quiet network, frames go out, no interrupt ---')
print(run(hunk, 'TEST', dict(unloaded, grow='noirq'))[0])
print('ALL OK')
