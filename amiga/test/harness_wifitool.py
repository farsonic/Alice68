#!/usr/bin/env python3
"""Run Alice68-WiFi on a 68k emulator (machine68k / Musashi) against a fake
alice68wifi.device and check what it does and says: plain status never starts
the chip, START shows each bring-up step, a stuck driver is reported instead
of hanging the Shell.

    harness_wifitool.py amiga/out/Alice68-WiFi
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M

R = M.Register
fails = []
CMD_STATUS, CMD_SCAN, CMD_LOG, CMD_RECONNECT, CMD_DISCONNECT, CMD_OFF, CMD_START = range(0x8a40, 0x8a47)
WS_OFF, WS_STARTING, WS_IDLE, WS_SCANNING, WS_JOINING, WS_CONNECTED, WS_RETRY, WS_FAILED = range(8)

def run(hunk, args, dev):
    """dev: dict(states = [(state, msg), ...] advanced on each STATUS after START,
    answer = commands the driver answers (others stay queued for ever), abort_ok)"""
    mach = M.Machine(M.CPUType.M68020, 4096)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    SYSBASE, DOSBASE, LOAD, HEAP, ARGS, STACK = 0x8000, 0xA000, 0x20000, 0x100000, 0x3F000, 0x80000
    st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'sent': [], 'pending': None, 'closed': False,
          'idx': 0, 'started': False, 'freed': False}
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
    lvo(DOSBASE, -48, lambda o, p: (st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')), setd0(d(R.D3))))
    lvo(DOSBASE, -198, lambda o, p: None)
    files = dev.get('files', {}); fhs = {}
    def dopen(o, p):
        name = mem.r_cstr(d(R.D1))
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
        ok = mem.r_cstr(d(R.A0)) == 'alice68wifi.device' and dev.get('present', True)
        mem.w8(d(R.A1) + 31, 0 if ok else 0xff); setd0(0 if ok else 0xffffffff)
    lvo(SYSBASE, -444, opendev)
    lvo(SYSBASE, -450, lambda o, p: st.__setitem__('closed', True))
    def cur():
        if not st['started']: return (WS_OFF, 'off - Alice68-WiFi START (or JOIN, or the TCP/IP stack) starts it')
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
        if cmd == CMD_RECONNECT: st['started'] = True
        if cmd == CMD_SCAN: mem.w32(io + 32, 0)
        return 0
    def doio(o, p):
        io = d(R.A1); cmd = mem.r16(io + 28)
        if cmd == CMD_STATUS: err = status(io)
        elif cmd == CMD_LOG:
            t = b'alice68wifi 1.9\nstep 1: reading the firmware files\n'; mem.w_block(mem.r32(io + 40), t); mem.w32(io + 32, len(t)); err = 0
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
steps = [(WS_STARTING, 'step 1: reading the firmware files'), (WS_STARTING, 'step 2: pins and power'),
         (WS_STARTING, 'step 3: SD host'), (WS_STARTING, 'step 4: chip'), (WS_STARTING, 'step 5: firmware download'),
         (WS_STARTING, 'step 6: firmware setup'), (WS_IDLE, 'ready')]
answer_all = (CMD_START, CMD_SCAN, CMD_RECONNECT, CMD_DISCONNECT, CMD_OFF)
cases = [
    # name, args, dev, wanted text, text that must not appear, commands sent, closed
    ('status never starts the chip', '', dict(states=steps, answer=answer_all),
     ['State:     off', 'Alice68-WiFi START'], ['Starting'], [], True),
    ('LOG never starts the chip', 'LOG', dict(states=steps, answer=answer_all),
     ['step 1: reading the firmware files'], [], [], True),
    ('START shows every step', 'START', dict(states=steps, answer=answer_all),
     ['Starting the WiFi chip:', 'step 1: reading', 'step 2: pins and power', 'step 3: SD host', 'step 4: chip',
      'step 5: firmware download', 'step 6: firmware setup', 'idle: ready'], ['stuck'], [CMD_START], True),
    ('START stopped by STEP=', 'START', dict(states=steps[:2] + [(WS_FAILED, 'stopped before step 3 (STEP= in the prefs)')], answer=answer_all),
     ['step 2: pins and power', 'failed: stopped before step 3', 'did not start'], [], [CMD_START], True),
    ('SCAN while off does not start it', 'SCAN', dict(states=steps, answer=answer_all),
     ['the WiFi chip is off: Alice68-WiFi START first'], [], [], True),
    ('stuck driver: reported, the Shell gets its prompt back', 'START', dict(states=steps, answer=()),
     ['the driver is stuck', 'Alice68-WiFi LOG in another Shell'], [], [CMD_START], False),
    ('queued request taken back', 'DISCONNECT', dict(states=steps, answer=(), abort_ok=True),
     ['the driver did not answer'], ['stuck'], [CMD_DISCONNECT], True),
    ('JOIN starts it and shows the steps', 'JOIN "Net" "secret1234"', dict(states=steps[:3] + [(WS_JOINING, 'joining "Net"'), (WS_CONNECTED, '')], answer=answer_all),
     ['Starting the WiFi chip:', 'step 2: pins and power', 'joining: joining "Net"', 'State:     connected'], [], [CMD_RECONNECT], True),
    ('READTEST reads without the driver', 'READTEST', dict(states=steps, files={'DEVS:Firmware/cyfmac43455-sdio.bin': bytes(100000),
                                                                             'DEVS:Firmware/brcmfmac43455-sdio.txt': b'x' * 2000}),
     ['32 KB reads', 'cyfmac43455-sdio.bin: opening... size... 100000 bytes', '.... ok, 100000 bytes',
      'brcmfmac43455-sdio.txt: opening... size... 2000 bytes', 'clm_blob: opening... not there', 'done: the machine still runs'], [], [], False),
    ('READTEST BIG: one read per file', 'READTEST BIG', dict(states=steps, files={'DEVS:Firmware/cyfmac43455-sdio.bin': bytes(100000)}),
     ['one read per file, into fast memory', '100000 bytes, memory... at $', 'reading...  ok, 100000 bytes'], [], [], False),
    ('missing device', '', dict(states=steps, present=False), ['cannot open alice68wifi.device'], [], [], False),
]
for name, args, dev, want, never, sent, closed in cases:
    out, st = run(hunk, args, dev)
    bad = [x for x in want if x not in out] + ['(not) ' + x for x in never if x in out]
    if bad: fails.append('%s: %s\n----\n%s----' % (name, bad, out))
    elif st['sent'] != sent: fails.append('%s: sent %s, want %s' % (name, [hex(c) for c in st['sent']], [hex(c) for c in sent]))
    elif st['closed'] != closed: fails.append('%s: device %sclosed' % (name, '' if st['closed'] else 'not '))
    elif not closed and dev.get('present', True) and st['freed']: fails.append('%s: freed a request the driver still holds' % name)
    else: print('ok  %s' % name)
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('--- sample: START ---')
print(run(hunk, 'START', dict(states=steps, answer=answer_all))[0])
print('ALL OK')
