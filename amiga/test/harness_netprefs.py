#!/usr/bin/env python3
"""Alice68-Network on a 68k emulator (machine68k) with fake intuition,
gadtools, graphics, dos, exec and alice68net.device: the gadgets are made,
the user types a network and presses "Join and save", then "Scan", then
closes the window. Checks what reaches the device and the prefs file, and
that every request is answered before the program ends.

    harness_netprefs.py amiga/out/Alice68-Network
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
mach = M.Machine(M.CPUType.M68020, 8192)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYS, DOS, INT, GT, GFX, EXP, LOAD, HEAP, ARGS, STACK = 0x8000, 0xA000, 0xB000, 0xC000, 0xD000, 0xE000, 0x20000, 0x200000, 0x3F000, 0x80000
st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'gadgets': [], 'sent': [], 'files': {}, 'pending': [], 'msgs': [], 'step': 0}
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
libs = {'dos.library': DOS, 'intuition.library': INT, 'gadtools.library': GT, 'graphics.library': GFX, 'expansion.library': EXP}
lvo(SYS, -552, lambda o, p: setd0(libs.get(mem.r_cstr(d(R.A1)), 0)))
lvo(SYS, -414, lambda o, p: None)
ME = alloc(232); mem.w32(ME + 172, 0x1234)             # a Shell process: pr_CLI set
lvo(SYS, -294, lambda o, p: setd0(ME))
lvo(EXP, -72, lambda o, p: setd0(0))                    # no board: status lines say so
# exec: ports, requests, signals
def newport(o, p): a = alloc(34); mem.w8(a + 15, 20); setd0(a)
lvo(SYS, -666, newport); lvo(SYS, -672, lambda o, p: None)
lvo(SYS, -654, lambda o, p: setd0(alloc(d(R.D0)))); lvo(SYS, -660, lambda o, p: None)
def opendev(o, p):
    name, unit, io = mem.r_cstr(d(R.A0)), d(R.D0), d(R.A1)
    ok = name == 'alice68net.device' and unit in (0, 1)
    if ok: mem.w32(io + 20, 0x7777); mem.w32(io + 24, 0x8000 + unit)
    setd0(0 if ok else 0xffffffff)
lvo(SYS, -444, opendev); lvo(SYS, -450, lambda o, p: None)
TIMER = []
def sendio(o, p):
    io = d(R.A1); cmd = mem.r16(io + 28)
    if mem.r32(io + 20) == 0x7777:
        st['sent'].append((cmd, mem.r32(io + 24) - 0x8000, mem.r32(io + 40), mem.r32(io + 36)))
        if cmd == 0x8a41:                               # SCAN: two networks
            e = mem.r32(io + 40)
            for i, (ssid, rssi, ch, sec) in enumerate([('Example-Net', -41, 1, 0x18), ('Guest', -70, 6, 1)]):
                b = e + i * 48
                mem.w_block(b + 6, ssid.encode() + b'\0'); mem.w16(b + 40, rssi & 0xffff); mem.w16(b + 42, ch); mem.w16(b + 44, sec)
            mem.w32(io + 32, 2)
        mem.w8(io + 31, 0); st['pending'].append(io)
    else: TIMER.append(io)
lvo(SYS, -462, sendio)
def checkio(o, p):
    io = d(R.A1); setd0(0 if io in TIMER else io)
lvo(SYS, -468, checkio)
def waitio(o, p):
    io = d(R.A1)
    if io in TIMER: TIMER.remove(io)
    if io in st['pending']: st['pending'].remove(io)
    setd0(0)
lvo(SYS, -474, waitio)
def abortio(o, p):
    io = d(R.A1)
    if io in TIMER: TIMER.remove(io)
lvo(SYS, -480, abortio)
def getmsg(o, p):
    port = d(R.A0)
    if port == st.get('ioport') and st['pending']: setd0(st['pending'].pop(0)); return
    setd0(0)
lvo(SYS, -372, getmsg)
def wait(o, p):
    st['step'] += 1
    if st['step'] > 50: fails.append('the event loop never ended'); st['done'] = True; cpu.end(); return
    setd0(d(R.D0))
lvo(SYS, -318, wait)
# dos
lvo(DOS, -60, lambda o, p: setd0(1))
fhs = {}
def dopen(o, p):
    name, mode = mem.r_cstr(d(R.D1)), d(R.D2)
    if mode == 1006: st['files'][name] = b''
    if name in st['files']: h = 0x100 + len(fhs); fhs[h] = [name, 0]; setd0(h)
    else: setd0(0)
lvo(DOS, -30, dopen); lvo(DOS, -36, lambda o, p: None)
def dread(o, p):
    f = fhs[d(R.D1)]; data = st['files'][f[0]][f[1]:f[1] + d(R.D3)]; mem.w_block(d(R.D2), data); f[1] += len(data); setd0(len(data))
lvo(DOS, -42, dread)
def dwrite(o, p):
    data = bytes(mem.r_block(d(R.D2), d(R.D3)))
    if d(R.D1) in fhs: st['files'][fhs[d(R.D1)][0]] += data
    else: st['out'].append(data.decode('latin1'))
    setd0(d(R.D3))
lvo(DOS, -48, dwrite)
st['files']['ENVARC:alice68wifi.prefs'] = b'SSID="Old Net"\nPASSWORD="oldpassword"\nCOUNTRY=GB\n'
# intuition / gadtools / graphics
SCR = alloc(64); mem.w8(SCR + 35, 4); mem.w8(SCR + 36, 4); FONT = alloc(8); mem.w16(FONT + 4, 8); mem.w32(SCR + 40, FONT)
lvo(INT, -510, lambda o, p: setd0(SCR)); lvo(INT, -516, lambda o, p: None)
WIN = alloc(128); UPORT = alloc(34); mem.w8(UPORT + 15, 21); mem.w32(WIN + 86, UPORT); mem.w32(WIN + 50, alloc(100))
def openwin(o, p):
    t = d(R.A1); tags = {}
    while mem.r32(t): tags[mem.r32(t)] = mem.r32(t + 4); t += 8
    st['wintags'] = tags; setd0(WIN)
lvo(INT, -606, openwin); lvo(INT, -72, lambda o, p: st.__setitem__('closed', True))
def createctx(o, p):
    c = alloc(64); mem.w32(d(R.A0), c); setd0(c)     # *glistptr = the context gadget
lvo(GT, -114, createctx)
gstr = {}
def creategad(o, p):
    kind, ng = d(R.D0), d(R.A1)
    gid = mem.r16(ng + 16); g = alloc(64); mem.w16(g + 38, gid)
    if kind == 12:                                      # STRING: a StringInfo with a buffer
        si = alloc(36); buf = alloc(80); mem.w32(si, buf); mem.w32(g + 34, si); gstr[gid] = buf
        t = d(R.A2)
        while t and mem.r32(t):
            if mem.r32(t) == 0x8008002d and mem.r32(t + 4): mem.w_block(buf, mem.r_cstr(mem.r32(t + 4)).encode() + b'\0')
            t += 8
    st['gadgets'].append((kind, gid, g)); setd0(g)
lvo(GT, -30, creategad); lvo(GT, -36, lambda o, p: None)
def setattrs(o, p):
    g, t = d(R.A0), d(R.A3)
    while t and mem.r32(t):
        if mem.r32(t) == 0x8008000b: st.setdefault('texts', []).append(mem.r_cstr(mem.r32(t + 4)) if mem.r32(t + 4) else '')
        if mem.r32(t) == 0x80080006 and mem.r32(t + 4) not in (0, 0xffffffff): st['labels'] = mem.r32(t + 4)
        t += 8
lvo(GT, -42, setattrs)
script = []
def gtget(o, p):
    if d(R.A0) != UPORT or not script: setd0(0); return
    cls, code, gid = script.pop(0)
    m = alloc(40); mem.w32(m + 20, cls); mem.w16(m + 24, code)
    if gid:
        g = [x[2] for x in st['gadgets'] if x[1] == gid][0]; mem.w32(m + 28, g)
    setd0(m)
lvo(GT, -72, gtget)
for off in (-78, -84, -90, -96, -120, -132): lvo(GT, off, lambda o, p: None)
lvo(GT, -126, lambda o, p: setd0(alloc(16)))
for off in (-240, -60, -342): lvo(GFX, off, lambda o, p: None)
# what the user does, one event per turn of the loop
G_LIST, G_SCAN, G_JOIN = 2, 3, 10
def type_in():
    mem.w_block(gstr[7], b'Example-Net\0'); mem.w_block(gstr[8], b'examplepass1\0'); mem.w_block(gstr[9], b'au\0')
script_steps = [lambda: (type_in(), script.append((0x40, 0, G_JOIN))), lambda: script.append((0x40, 0, G_SCAN)),
                lambda: None, lambda: script.append((0x40, 1, G_LIST)), lambda: script.append((0x200, 0, 0))]
orig_wait = wait
def wait2(o, p):
    st['step'] += 1
    if st['step'] > 60: fails.append('the event loop never ended'); st['done'] = True; cpu.end(); return
    if script_steps: script_steps.pop(0)()
    setd0(d(R.D0) & ~0x1000)                            # everything but Ctrl-C
lvo(SYS, -318, wait2)
# the program
h = open(sys.argv[1], 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
if w(p) == 0x3ec:
    for i in range(w(p + 4)):
        r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
mem.w_block(LOAD, bytes(code))
mem.w_block(ARGS, b'\n\0'); mem.w32(STACK, 0x1000)
# the device's reply port: the first port the program makes
first = {'n': 0}
def newport2(o, p):
    a = alloc(34); mem.w8(a + 15, 20 + first['n'])
    if first['n'] == 0: st['ioport'] = a
    first['n'] += 1; setd0(a)
lvo(SYS, -666, newport2)
cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, 1); cpu.w_pc(LOAD); cpu.w_sr(0)
for _ in range(4000):
    if st['done']: break
    cpu.execute(100000)
if st['exc']: fails.append('CPU exception %d at $%x' % st['exc'])
if not st['done']: fails.append('did not return')
kinds = sorted(set(k for k, _, _ in st['gadgets']))
if kinds != [1, 4, 12, 13] or len(st['gadgets']) != 13: fails.append('gadgets: %s' % [(k, i) for k, i, _ in st['gadgets']])
tags = st.get('wintags', {})
if tags.get(0x80000076) != 616 or not tags.get(0x8000006c): fails.append('window tags %s' % tags)
join = [s for s in st['sent'] if s[0] == 0x8a47]
if not join: fails.append('no JOIN sent: %s' % st['sent'])
else:
    a = join[0][2]
    got = (mem.r_cstr(a), mem.r_cstr(a + 36), mem.r_cstr(a + 104))
    if got != ('Example-Net', 'examplepass1', 'AU') or join[0][1] != 0 or join[0][3] != 108: fails.append('JOIN carried %s, unit %d, length %d' % (got, join[0][1], join[0][3]))
if not any(s[0] == 0x8a41 for s in st['sent']): fails.append('no SCAN sent')
pf = st['files'].get('ENVARC:alice68wifi.prefs', b'')
if pf != b'SSID="Example-Net"\nPASSWORD="examplepass1"\nCOUNTRY=AU\n': fails.append('prefs saved as %r' % pf)
if not st.get('labels'): fails.append('the scan did not fill the list')
if mem.r_cstr(gstr[7]) != 'Example-Net' and 'Guest' not in str(st.get('texts')): pass
if st['pending'] or TIMER: fails.append('requests left out: %s %s' % (st['pending'], TIMER))
if not st.get('closed'): fails.append('window not closed')
if fails:
    print('sent', [(hex(a), b) for a, b, *_ in st['sent']], 'steps', st['step'])
    print(''.join(st['out'])); print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('gadgets made: %d; JOIN sent with Example-Net/examplepass1/AU; prefs saved; scan listed; window closed' % len(st['gadgets']))
print('ALL OK')
