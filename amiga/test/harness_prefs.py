#!/usr/bin/env python3
"""Alice68Prefs (the MUI editor) on a 68k emulator (machine68k) without MUI: muimaster.library
does not open. From the Shell it must say that it needs MUI 3.8 and point to the Alice68
command; from Workbench the same in a requester, and reply to Workbench's message. A wrong
argument line is reported by PrintFault. Every library it opened must be closed again.
Started with a 4 KB stack, it runs on a 64 KB one of its own (StackSwap) and frees it.
(The pages' logic is tested through the Shell command: harness_cardcmd.py, harness_status.py,
harness_usblog.py, harness_joytest.py, harness_monitor.py, harness_wifitool.py.)

    harness_prefs.py amiga/out/Alice68Prefs
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
SYS, DOS, INT, GFX, LOAD, STACK, PROC, WBMSG = 0x8000, 0xA000, 0xB000, 0xC000, 0x40000, 0x3F000, 0x1E000, 0x1F000
hunk = open(sys.argv[1], 'rb').read()

def load(mem):
    """HUNK_HEADER, then CODE/DATA/BSS hunks with RELOC32: placed one after another from LOAD"""
    w = lambda o: struct.unpack('>I', hunk[o:o + 4])[0]
    assert w(0) == 0x3f3 and w(4) == 0
    n, first, last = w(8), w(12), w(16)
    sizes = [w(20 + 4 * i) & 0x3fffffff for i in range(n)]
    base, a = [], LOAD
    for s in sizes: base.append(a); a = (a + s * 4 + 8 + 7) & ~7
    p, h = 20 + 4 * n, 0
    blocks = {}
    while p < len(hunk) and h < n:
        t = w(p) & 0x3fffffff
        if t in (0x3e9, 0x3ea):
            k = w(p + 4) * 4; blocks[h] = bytearray(hunk[p + 8:p + 8 + k]) + bytes(sizes[h] * 4 - k); p += 8 + k
        elif t == 0x3eb:
            blocks[h] = bytearray(sizes[h] * 4); p += 8
        elif t == 0x3ec:
            p += 4
            while w(p):
                cnt, to = w(p), w(p + 4); p += 8
                for i in range(cnt):
                    off = w(p); p += 4
                    v = struct.unpack('>I', blocks[h][off:off + 4])[0]; blocks[h][off:off + 4] = struct.pack('>I', v + base[to])
            p += 4
        elif t == 0x3f2:
            p += 4; h += 1
        else:
            raise SystemExit('hunk type %x' % t)
    for i, b in blocks.items(): mem.w_block(base[i], bytes(b))
    return base[0], a

def run(cli, args='\n', rdargs_ok=True):
    mach = M.Machine(M.CPUType.M68020, 8192)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'out': [], 'opened': [], 'easy': None, 'replied': None, 'fault': None, 'rc': None, 'wait': 0}
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
    libs = {'dos.library': DOS, 'intuition.library': INT, 'graphics.library': GFX}
    def openlib(o, p):
        name = mem.r_cstr(d(R.A1)); b = libs.get(name, 0)
        if b: st['opened'].append(b)
        st.setdefault('asked', []).append(name)
        setd0(b)
    lvo(SYS, -552, openlib)
    def closelib(o, p):
        if d(R.A1) in st['opened']: st['opened'].remove(d(R.A1))
        else: fails.append('CloseLibrary(%x): not open' % d(R.A1))
    lvo(SYS, -414, closelib)
    mem.w32(PROC + 172, 0x1234 if cli else 0)
    lvo(SYS, -294, lambda o, p: setd0(PROC if d(R.A1) == 0 else 0))
    def waitport(o, p): st['wait'] += 1; setd0(WBMSG)
    lvo(SYS, -384, waitport)
    lvo(SYS, -372, lambda o, p: setd0(WBMSG if not cli and d(R.A0) == PROC + 92 else 0))
    def reply(o, p): st['replied'] = d(R.A1)
    lvo(SYS, -378, reply)
    for off in (-132, -138): lvo(SYS, off, lambda o, p: None)
    mem.w32(PROC + 58, STACK - 0x1000); mem.w32(PROC + 62, STACK + 4)    # tc_SPLower/Upper: a Shell's 4 KB
    BIG = 0x100000
    def allocmem(o, p):
        if st.get('mem'): fails.append('AllocMem asked twice'); setd0(0); return
        st['mem'] = (BIG, d(R.D0)); setd0(BIG)
    lvo(SYS, -198, allocmem)
    def freemem(o, p): st['freedmem'] = (d(R.A1), d(R.D0))
    lvo(SYS, -210, freemem)
    def stackswap(o, p):
        # exec: the return address moves from the old stack to the new one, the old sp is kept in stk_Pointer
        ss, sp = d(R.A0), cpu.r_reg(R.A7); ret = mem.r32(sp); new = mem.r32(ss + 8)
        lo, hi = mem.r32(ss), mem.r32(ss + 4)
        mem.w32(ss, mem.r32(PROC + 58)); mem.w32(ss + 4, mem.r32(PROC + 62)); mem.w32(ss + 8, sp + 4)
        mem.w32(PROC + 58, lo); mem.w32(PROC + 62, hi)
        mem.w32(new - 4, ret); cpu.w_reg(R.A7, new - 4)
        st.setdefault('swaps', []).append((lo, hi, new))
    lvo(SYS, -732, stackswap)
    mem.w32(WBMSG + 28, 0)                       # sm_NumArgs 0: no icon to read
    lvo(DOS, -798, lambda o, p: setd0(0x1D000 if rdargs_ok else 0))
    lvo(DOS, -858, lambda o, p: st.__setitem__('freed', d(R.D1)))
    def fault(o, p): st['fault'] = mem.r_cstr(d(R.D2)); setd0(0)
    lvo(DOS, -474, fault)
    lvo(DOS, -132, lambda o, p: setd0(116))
    lvo(DOS, -60, lambda o, p: setd0(1 if cli else 0))
    def write(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); setd0(d(R.D3))
    lvo(DOS, -48, write)
    def easy(o, p):
        es = d(R.A1)
        st['easy'] = (mem.r_cstr(mem.r32(es + 8)), mem.r_cstr(mem.r32(es + 12)), mem.r_cstr(mem.r32(es + 16))); setd0(1)
    lvo(INT, -588, easy)
    entry, end = load(mem)
    if end > 0x200000: fails.append('the program is larger than the harness expects (%x)' % end)
    a = args.encode()
    mem.w_block(0x3E000, a + b'\0'); mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, 0x3E000); cpu.w_reg(R.D0, len(a)); cpu.w_pc(entry); cpu.w_sr(0)
    for _ in range(200):
        if st['done']: break
        cpu.execute(100000)
    if st['exc']: fails.append('CPU exception %d at $%x' % st['exc'])
    if not st['done']: fails.append('did not return')
    if st['opened']: fails.append('libraries left open: %s' % [hex(x) for x in st['opened']])
    sw = st.get('swaps', [])
    if st.get('mem', (0, 0))[1] < 65536 or len(sw) != 2 or not (BIG < sw[0][0] < sw[0][1] == sw[0][2] == BIG + st['mem'][1]) \
       or sw[1][:2] != (STACK - 0x1000, STACK + 4) or not STACK - 0x1000 < sw[1][2] < STACK or st.get('freedmem') != st.get('mem') or cpu.r_reg(R.A7) != STACK + 4:
        fails.append('own stack: alloc %r, swaps %r, freed %r, sp %x' % (st.get('mem'), sw, st.get('freedmem'), cpu.r_reg(R.A7)))
    return st

st = run(True)
out = ''.join(st['out'])
if 'needs MUI 3.8 or later (muimaster.library 19)' not in out or 'Alice68 HELP' not in out: fails.append('Shell: %r' % out)
if 'muimaster.library' not in st.get('asked', []): fails.append('muimaster.library not asked for: %s' % st.get('asked'))
if st.get('freed') != 0x1D000: fails.append('ReadArgs\' result not freed')
if st['wait'] or st['replied']: fails.append('Shell start waited for or replied to a Workbench message')
st = run(False)
if not st['easy'] or 'needs MUI 3.8' not in st['easy'][1] or 'Alice68 HELP' not in st['easy'][1]: fails.append('Workbench requester %r' % (st['easy'],))
if st['out']: fails.append('Workbench start wrote to a Shell: %r' % st['out'])
if st['replied'] != WBMSG: fails.append('Workbench\'s message not replied (%r)' % st['replied'])
st = run(True, 'PAGE\n', rdargs_ok=False)
if st['fault'] != 'Alice68Prefs' or st['rc'] != 10 or 'muimaster.library' in st.get('asked', []): fails.append('a wrong line: fault %r rc %r' % (st['fault'], st['rc']))
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('without MUI: the Shell is told (and pointed to Alice68 HELP), Workbench gets a requester and its message back; '
      'a wrong line goes to PrintFault; every library closed')
print('ALL OK')
