#!/usr/bin/env python3
"""Run Alice68-WiFi on a 68k emulator (machine68k / Musashi) against a fake
alice68net.device (the kernel's WiFi) and check what it does and says: status
and LOG, START, JOIN sending the network to the kernel, RECONNECT taking it
from the prefs, and a stuck driver reported instead of hanging the Shell.

    harness_wifitool.py amiga/out/Alice68-WiFi
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M

R = M.Register
fails = []
CMD_STATUS, CMD_SCAN, CMD_LOG, CMD_RECONNECT, CMD_DISCONNECT, CMD_OFF, CMD_START, CMD_JOIN = range(0x8a40, 0x8a48)
WS_OFF, WS_STARTING, WS_IDLE, WS_SCANNING, WS_JOINING, WS_CONNECTED, WS_RETRY, WS_FAILED, WS_NOFW = range(9)

def run(hunk, args, dev):
    """dev: dict(states = [(state, msg), ...] advanced on each STATUS after START,
    answer = commands the driver answers (others stay queued for ever), abort_ok)"""
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    SYSBASE, DOSBASE, LOAD, HEAP, ARGS, STACK = 0x8000, 0xA000, 0x20000, 0x100000, 0x3F000, 0x80000
    st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'sent': [], 'pending': None, 'closed': False,
          'idx': 0, 'started': dev.get('running', False), 'freed': False, 'join': None}
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
    lvo(SYSBASE, -552, lambda o, p: setd0(DOSBASE if mem.r_cstr(d(R.A1)) == 'dos.library' else 0))
    lvo(SYSBASE, -414, lambda o, p: None)
    lvo(SYSBASE, -666, lambda o, p: setd0(alloc(34)))
    lvo(SYSBASE, -672, lambda o, p: st.__setitem__('freed', True))
    lvo(SYSBASE, -654, lambda o, p: setd0(alloc(d(R.D0))))
    lvo(SYSBASE, -660, lambda o, p: st.__setitem__('freed', True))
    lvo(SYSBASE, -306, lambda o, p: setd0(0))
    lvo(DOSBASE, -60, lambda o, p: setd0(1))
    def dwrite(o, p):
        data = bytes(mem.r_block(d(R.D2), d(R.D3)))
        if d(R.D1) in fhs: files[fhs[d(R.D1)][0]] += data
        else: st['out'].append(data.decode('latin1'))
        setd0(d(R.D3))
    lvo(DOSBASE, -48, dwrite)
    lvo(DOSBASE, -198, lambda o, p: None)
    files = dev.get('files', {}); fhs = {}
    def dopen(o, p):
        name = mem.r_cstr(d(R.D1))
        if d(R.D2) == 1006: files[name] = b''; st['saved'] = name           # MODE_NEWFILE
        if name in files: h = 0x100 + len(fhs); fhs[h] = [name, 0]; setd0(h)
        else: setd0(0)
    lvo(DOSBASE, -30, dopen)
    lvo(DOSBASE, -36, lambda o, p: fhs.pop(d(R.D1), None))
    def dseek(o, p):
        f = fhs[d(R.D1)]; old = f[1]; pos, mode = d(R.D2), d(R.D3)
        f[1] = len(files[f[0]]) if mode == 1 else pos; setd0(old)
    lvo(DOSBASE, -66, dseek)
    def dread(o, p):
        f = fhs[d(R.D1)]; data = files[f[0]][f[1]:f[1] + d(R.D3)]
        mem.w_block(d(R.D2), data); f[1] += len(data); st['reads'] = st.get('reads', 0) + 1; setd0(len(data))
    lvo(DOSBASE, -42, dread)
    lvo(SYSBASE, -198, lambda o, p: setd0(alloc(d(R.D0))))
    lvo(SYSBASE, -210, lambda o, p: None)
    def opendev(o, p):
        ok = mem.r_cstr(d(R.A0)) == 'alice68net.device' and dev.get('present', True)
        mem.w8(d(R.A1) + 31, 0 if ok else 0xff); setd0(0 if ok else 0xffffffff)
    lvo(SYSBASE, -444, opendev)
    lvo(SYSBASE, -450, lambda o, p: st.__setitem__('closed', True))
    def cur():
        if not st['started']: return dev.get('initial', (WS_OFF, 'switched off'))
        s = dev['states']; v = s[min(st['idx'], len(s) - 1)]; st['idx'] += 1; return v
    def status(io):
        a = mem.r32(io + 40)
        if mem.r32(io + 36) < 292: return 3
        state, msg = cur()
        mem.w_block(a, bytes(292)); mem.w16(a, 1); mem.w16(a + 2, state)
        mem.w_block(a + 196, msg.encode()[:95] + b'\0')
        mem.w32(io + 32, 292); return 0
    def finish(io, cmd):
        if cmd == CMD_START: st['started'] = True
        if cmd in (CMD_RECONNECT, CMD_JOIN):
            st['started'] = True
            a = mem.r32(io + 40)
            st['join'] = (mem.r_cstr(a), mem.r_cstr(a + 36), mem.r_cstr(a + 104)) if a else None
        if cmd == CMD_SCAN: mem.w32(io + 32, 0)
        return 0
    def doio(o, p):
        io = d(R.A1); cmd = mem.r16(io + 28)
        if cmd == CMD_STATUS: err = status(io)
        elif cmd == CMD_LOG:
            t = b'starting the WiFi chip\nchip 4345 rev 6: cyfmac43455-sdio-standard.bin\n'; mem.w_block(mem.r32(io + 40), t); mem.w32(io + 32, len(t)); err = 0
        else:
            fails.append('DoIO of command %x (the process may be stuck: SendIO with a timeout)' % cmd); err = 0
        mem.w8(io + 31, err); setd0(err)
    lvo(SYSBASE, -456, doio)
    def sendio(o, p):
        io = d(R.A1); cmd = mem.r16(io + 28); st['sent'].append(cmd)
        if cmd in dev.get('answer', ()): mem.w8(io + 31, finish(io, cmd)); st['pending'] = None
        else: st['pending'] = io
    lvo(SYSBASE, -462, sendio)
    lvo(SYSBASE, -468, lambda o, p: setd0(0 if st['pending'] == d(R.A1) else d(R.A1)))
    def waitio(o, p):
        if st['pending'] == d(R.A1): fails.append('WaitIO on a request the driver never answers'); st['done'] = True; cpu.end()
        setd0(mem.r8(d(R.A1) + 31))
    lvo(SYSBASE, -474, waitio)
    def abortio(o, p):
        if st['pending'] == d(R.A1) and dev.get('abort_ok'): st['pending'] = None; mem.w8(d(R.A1) + 31, 0xfe)
    lvo(SYSBASE, -480, abortio)
    h = open(hunk, 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    mem.w_block(ARGS, args.encode() + b'\n\0')
    mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(args) + 1); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(4000):
        if st['done']: break
        cpu.execute(100000)
    if st['exc']: fails.append('CPU exception %d at $%x' % st['exc'])
    if not st['done']: fails.append('did not return')
    return ''.join(st['out']), st

hunk = sys.argv[1]
steps = [(WS_STARTING, 'starting the WiFi chip'), (WS_IDLE, 'ready')]
joining = [(WS_SCANNING, 'ready'), (WS_JOINING, 'joining Net'), (WS_CONNECTED, 'connected to Net')]
answer_all = (CMD_START, CMD_SCAN, CMD_RECONNECT, CMD_DISCONNECT, CMD_OFF, CMD_JOIN)
prefs = {'ENV:alice68wifi.prefs': b'SSID="Home Net"\nPASSWORD="secret12345"\nCOUNTRY=GB\n'}
cases = [
    # name, args, dev, wanted text, text that must not appear, commands sent, closed
    ('status: what the kernel says', '', dict(states=steps, answer=answer_all, initial=(WS_NOFW, 'waiting for the firmware files (WIFI folder on the SD card)')),
     ['State:     no firmware', 'WIFI folder on the SD card'], ['Starting'], [], True),
    ('LOG: the kernel network log', 'LOG', dict(states=steps, answer=answer_all),
     ['chip 4345 rev 6'], [], [], True),
    ('START after OFF shows it starting', 'START', dict(states=steps, answer=answer_all),
     ['Starting the WiFi chip:', 'starting: starting the WiFi chip', 'idle: ready'], ['stuck'], [CMD_START], True),
    ('START while running does nothing', 'START', dict(states=steps, answer=answer_all, running=True),
     ['already running'], [], [], True),
    ('SCAN while off does not start it', 'SCAN', dict(states=steps, answer=answer_all),
     ['the WiFi chip is off: Alice68-WiFi START first'], [], [], True),
    ('JOIN sends the network to the kernel', 'JOIN "Net" "secret1234" GB', dict(states=joining, answer=answer_all),
     ['Saved in ENV:', 'joining: joining Net', 'State:     connected'], [], [CMD_JOIN], True),
    ('RECONNECT takes the network from the prefs', 'RECONNECT', dict(states=joining, answer=answer_all, files=prefs),
     ['State:     connected'], ['no SSID'], [CMD_JOIN], True),
    ('RECONNECT without prefs: the last network', 'RECONNECT', dict(states=joining, answer=answer_all),
     ['no SSID in ENV:alice68wifi.prefs', 'State:     connected'], [], [CMD_RECONNECT], True),
    ('stuck driver: reported, the Shell gets its prompt back', 'JOIN "Net" "secret1234"', dict(states=steps, answer=()),
     ['the driver is stuck', 'Alice68-WiFi LOG in another Shell'], [], [CMD_JOIN], False),
    ('queued request taken back', 'DISCONNECT', dict(states=steps, answer=(), abort_ok=True),
     ['the driver did not answer'], ['stuck'], [CMD_DISCONNECT], True),
    ('missing device', '', dict(states=steps, present=False), ['cannot open alice68net.device'], [], [], False),
]
for name, args, dev, want, never, sent, closed in cases:
    out, st = run(hunk, args, dev)
    bad = [x for x in want if x not in out] + ['(not) ' + x for x in never if x in out]
    if bad: fails.append('%s: %s\n----\n%s----' % (name, bad, out))
    elif st['sent'] != sent: fails.append('%s: sent %s, want %s' % (name, [hex(c) for c in st['sent']], [hex(c) for c in sent]))
    elif st['closed'] != closed: fails.append('%s: device %sclosed' % (name, '' if st['closed'] else 'not '))
    elif not closed and dev.get('present', True) and st['freed']: fails.append('%s: freed a request the driver still holds' % name)
    elif name.startswith('JOIN sends') and st['join'] != ('Net', 'secret1234', 'GB'): fails.append('%s: kernel got %s' % (name, st['join']))
    elif name.startswith('RECONNECT takes') and st['join'] != ('Home Net', 'secret12345', 'GB'): fails.append('%s: kernel got %s' % (name, st['join']))
    else: print('ok  %s' % name)
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('--- sample: START ---')
print(run(hunk, 'START', dict(states=steps, answer=answer_all))[0])
print('ALL OK')
