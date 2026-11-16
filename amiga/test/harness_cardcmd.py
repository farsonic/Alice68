#!/usr/bin/env python3
"""`Alice68 CARD` and `Alice68 BT` (amiga/cardcmd.c on cardcore.c, the code Alice68Prefs's Card,
Files, Settings, Text files and Bluetooth pages use) on a 68k emulator (machine68k), against
alice68hdf.device's card service and the network board, both faked here (the card held in
Python). Goes through what Alice68-Card's harness did: partitions, mounted files, free space;
settings and the Kickstart saved with .bak; delete, new folder, rename, an in-use hardfile
refused, boot from a hardfile; config.txt lines edited; the WiFi password never shown and kept
when the line is typed with ********; Bluetooth scan, pair, keep (bt.dev=), forget; restart.
Every run must give back its memory and close the device.

    harness_cardcmd.py amiga/out/Alice68
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
SYS, DOS, EXP, LOAD, HEAP, ARGS, STACK = 0x8000, 0xA000, 0xE000, 0x20000, 0x200000, 0x6F000, 0x80000
card = {
    'cmdline.txt': b'coherent_pool=1M aga.look=0 aga.scale=1 eth=off hdf.boot=Prism wifi.ssid="Home Net" wifi.psk="old secret" net.trace=1\n',
    'config.txt': b'# the Pi\n[pi4]\narm_64bit=1\n[all]\ninitramfs kick31.rom followkernel\nalice68.folder=Games\n',
    'kick31.rom': bytes(524288), 'kick32.rom': bytes(524288), 'kernel8.img': bytes(5000), 'old.txt': b'old\n',
    'HDF/': None, 'HDF/Prism.hdf': bytes(4096), 'HDF/Spare.hdf': bytes(2048), 'ISO/': None,
}
INUSE = {'HDF/Prism.hdf'}
calls = []
board = bytearray(0x4000); board[0:8] = struct.pack('>II', 0x4136384e, 1)
BT = 0x3800
hunk = open(sys.argv[1], 'rb').read()

def ents(folder):
    pre = folder + '/' if folder else ''
    return [k[len(pre):] for k in card if k.startswith(pre) and k[len(pre):] and '/' not in k[len(pre):].rstrip('/')]

def run(args):
    mach = M.Machine(M.CPUType.M68020, 8192)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'allocs': {}, 'opened': [], 'rc': None}
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
    def alloc(n):
        a = (st['heap'] + 7) & ~7; st['heap'] = a + n; mem.w_block(a, bytes(n)); return a
    libs = {'dos.library': DOS, 'expansion.library': EXP}
    lvo(SYS, -552, lambda o, p: setd0(libs.get(mem.r_cstr(d(R.A1)), 0)))
    lvo(SYS, -414, lambda o, p: None)
    ME = alloc(232); mem.w32(ME + 172, 0x1234)
    lvo(SYS, -294, lambda o, p: setd0(ME))
    def allocmem(o, p): a = alloc(d(R.D0)); st['allocs'][a] = d(R.D0); setd0(a)
    def freemem(o, p):
        a, n = d(R.A1), d(R.D0)
        if st['allocs'].get(a) != n: fails.append('%s: FreeMem(%x, %d): allocated %s' % (args, a, n, st['allocs'].get(a)))
        st['allocs'].pop(a, None)
    lvo(SYS, -198, allocmem); lvo(SYS, -210, freemem)
    for off in (-132, -138): lvo(SYS, off, lambda o, p: None)
    def newport(o, p): a = alloc(34); st['allocs'][a] = 'port'; setd0(a)
    def delport(o, p): st['allocs'].pop(d(R.A0), None)
    lvo(SYS, -666, newport); lvo(SYS, -672, delport)
    def newio(o, p): a = alloc(d(R.D0)); st['allocs'][a] = 'io'; setd0(a)
    def delio(o, p): st['allocs'].pop(d(R.A0), None)
    lvo(SYS, -654, newio); lvo(SYS, -660, delio)
    def opendev(o, p):
        name, unit, io = mem.r_cstr(d(R.A0)), d(R.D0), d(R.A1)
        ok = name == 'alice68hdf.device' and unit == 0x41363843
        if ok: mem.w32(io + 20, 0x6666); st['opened'].append(io)
        setd0(0 if ok else 0xffffffff)
    lvo(SYS, -444, opendev)
    lvo(SYS, -450, lambda o, p: st['opened'].remove(d(R.A1)) if d(R.A1) in st['opened'] else fails.append('CloseDevice of something not open'))
    def cs_do(io):
        cmd, data, length, off = mem.r16(io + 28), mem.r32(io + 40), mem.r32(io + 36), mem.r32(io + 44)
        err = 0; actual = 0
        if cmd == 9:
            mem.w_block(data, bytes(length))
            mem.w32(data, 1); mem.w32(data + 8, 1); mem.w32(data + 12, 2)
            for i, (typ, kind, start, count, vol, label) in enumerate([(0x0c, 2, 8192, 524288, 0, 'ALICE68'), (0x76, 4, 532480, 1048576, 0xffffffff, '')]):
                pb = data + 16 + 48 * i
                for k, v in enumerate((typ, kind, 1 if i == 0 else 0, start, count, vol, count // 2, 100000 if off & 1 else 0xffffffff)): mem.w32(pb + 4 * k, v)
                mem.w_block(pb + 32, label.encode() + b'\0')
            ib = data + 16 + 48 * 8
            mem.w32(ib, 0); mem.w_block(ib + 4, b'no exFAT partition in use\0'); mem.w_block(ib + 84, b'brcm-emmc.device\0')
            mem.w32(ib + 116, 1); mem.w32(ib + 120, 0); mem.w32(ib + 124, 1); mem.w_block(ib + 128, b'Prism.hdf\0')
            calls.append(('INFO', off))
        elif cmd == 10:
            vol, path, mx = mem.r32(data), mem.r_cstr(data + 4), mem.r32(data + 260)
            if vol != 0: err = 9
            elif path and path + '/' not in card: err = 1
            else:
                es = ents(path); n = 0
                for name in es[:mx]:
                    e = data + 272 + 124 * n; full = (path + '/' if path else '') + name
                    isdir = name.endswith('/')
                    mem.w_block(e, name.rstrip('/').encode() + b'\0')
                    mem.w32(e + 108, 0 if isdir else len(card[full])); mem.w32(e + 112, 0); mem.w32(e + 116, 0)
                    mem.w32(e + 120, (1 if isdir else 0) | (2 if full in INUSE else 0)); n += 1
                mem.w32(data + 264, n); mem.w32(data + 268, 1 if len(es) > mx else 0); actual = n
            calls.append(('LIST', path))
        else:
            vol, path, path2, ln, mx = mem.r32(data), mem.r_cstr(data + 4), mem.r_cstr(data + 260), mem.r32(data + 516), mem.r32(data + 520)
            name = {11: 'READ', 12: 'WRITE', 13: 'DELETE', 14: 'RENAME', 15: 'MKDIR'}[cmd]
            calls.append((name, path) + ((path2,) if cmd == 14 else ()))
            if cmd == 11:
                if path not in card: err = 1
                elif card[path] is None: err = 11
                else:
                    b = card[path]; mem.w32(data + 516, len(b))
                    if len(b) > mx: err = 10
                    else: mem.w_block(data + 524, b); actual = len(b)
            elif cmd == 12: card[path] = bytes(mem.r_block(data + 524, ln)); actual = ln
            elif cmd == 13:
                if path in INUSE: err = 4
                elif path in card: del card[path]
                elif path + '/' in card:
                    if ents(path): err = 8
                    else: del card[path + '/']
                else: err = 1
            elif cmd == 14:
                if path in INUSE: err = 4
                elif path in card: card[path2] = card.pop(path)
                else: err = 1
            elif cmd == 15:
                if path + '/' in card: err = 2
                else: card[path + '/'] = None
        mem.w32(io + 32, actual); mem.w8(io + 31, err)
        return err
    def doio(o, p):
        io = d(R.A1)
        if mem.r32(io + 20) != 0x6666: fails.append('DoIO on a request not opened'); setd0(0xff); return
        setd0(cs_do(io))
    lvo(SYS, -456, doio)
    BOARD = alloc(0x4000); mem.w_block(BOARD, bytes(board))
    CD = alloc(48); mem.w32(CD + 32, BOARD)
    lvo(EXP, -72, lambda o, p: setd0(CD if (d(R.D0), d(R.D1)) == (0x6d73, 0x6b) else 0))
    lvo(DOS, -60, lambda o, p: setd0(1))
    def write(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); setd0(d(R.D3))
    lvo(DOS, -48, write)
    w = lambda o: struct.unpack('>I', hunk[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(hunk[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    a = args.encode('latin1') + b'\n'
    mem.w_block(ARGS, a + b'\0'); mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(a)); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(400):
        if st['done']: break
        cpu.execute(100000)
    if st['exc']: fails.append('%s: CPU exception %d at $%x' % (args, *st['exc']))
    if not st['done']: fails.append('%s: did not return' % args)
    if st['opened']: fails.append('%s: the card service left open' % args)
    if st['allocs']: fails.append('%s: memory not given back: %s' % (args, st['allocs']))
    board[:] = bytes(mem.r_block(BOARD, 0x4000))
    return ''.join(st['out']), st['rc']

def chk(c, m):
    if not c: fails.append(m)
def bt_ready():
    lg = b'WiFi line\n[BT] controller: HCI 7\n'
    board[0x2000:0x2000 + len(lg)] = lg; board[24:28] = struct.pack('>I', len(lg))
    board[BT:BT + 4] = struct.pack('>I', 3); board[BT + 56:BT + 62] = b'ready\0'; board[BT + 28:BT + 32] = struct.pack('>I', 1)
    board[BT + 560:BT + 566] = bytes([0x11, 0x12, 0x13, 0x14, 0x15, 0x16]); board[BT + 568:BT + 572] = struct.pack('>I', 0x002540)
    board[BT + 576:BT + 586] = b'Slim Keys\0'
    board[BT + 572:BT + 576] = struct.pack('>i', -58)
def bt_paired():
    def put(o, b): board[BT + o:BT + o + len(b)] = b
    put(24, struct.pack('>I', 3))
    put(128, bytes([0x11, 0x12, 0x13, 0x14, 0x15, 0x16])); put(136, bytes(range(16))); put(152, struct.pack('>II', 4, 3)); put(160, b'Slim Keys\0')
    put(200, bytes([0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 1, 1])); put(208, bytes(range(0x20, 0x30))); put(224, struct.pack('>II', 0, 8 | 4 | 1)); put(232, b'LE Mouse\0')
    put(1260, bytes(range(1, 9))); put(1268, struct.pack('>I', 0x1234)); put(1272, bytes(range(0x40, 0x50)))
    put(272, bytes([0x31, 0x32, 0x33, 0x34, 0x35, 0x36])); put(280, bytes(range(0x30, 0x40))); put(296, struct.pack('>II', 4, 16 | 1)); put(304, b'EOS Mini\0')

o, rc = run('CARD')
chk(rc == 0 and 'brcm-emmc.device' in o and 'ALICE68' in o and 'FAT32' in o and "(Alice68's FAT)" in o and 'Amiga (RDB)' in o, 'CARD: %r' % o)
chk('hardfile' in o and 'Prism.hdf' in o and ('INFO', 0) in calls, 'CARD: mounted files %r' % o)
o, rc = run('CARD INFO FREE')
chk(('INFO', 1) in calls and rc == 0, 'INFO FREE: free space not counted')
o, rc = run('CARD LIST')
names = [l.split()[0] for l in o.splitlines()[1:]]
chk(names[:3] == ['HDF/', 'ISO/', 'cmdline.txt'] and 'kick31.rom' in o and 'Kickstart?' in o, 'LIST sorted, folders first: %r' % o)
o, rc = run('CARD OPTIONS')
chk(rc == 0 and '********' in o and 'old secret' not in o and 'Scaling' in o and 'Country' in o, 'OPTIONS: %r' % o)
o, rc = run('card set Scaling=16:9 wifi.country=gb')
chk(rc == 0 and 'Saved cmdline.txt' in o, 'SET: %r' % o)
chk(card['cmdline.txt'] == b'coherent_pool=1M aga.look=0 eth=off hdf.boot=Prism wifi.ssid="Home Net" wifi.psk="old secret" net.trace=1 aga.scale=4 wifi.country=GB\n',
    'cmdline.txt saved as %r' % card['cmdline.txt'])
chk(card.get('cmdline.txt.bak', b'').startswith(b'coherent_pool=1M aga.look=0 aga.scale=1'), 'cmdline.txt.bak %r' % card.get('cmdline.txt.bak'))
o, rc = run('CARD SET nosuch=1')
chk(rc == 10 and 'no such option' in o, 'SET of an unknown option: %r' % o)
o, rc = run('CARD KICKSTART')
chk('* kick31.rom' in o and 'kick32.rom' in o, 'KICKSTART list: %r' % o)
o, rc = run('CARD KICKSTART kick32.rom')
chk(b'initramfs kick32.rom followkernel\n' in card['config.txt'] and b'kick31' not in card['config.txt'], 'config.txt Kickstart: %r' % card['config.txt'])
chk(card.get('config.txt.bak') == b'# the Pi\n[pi4]\narm_64bit=1\n[all]\ninitramfs kick31.rom followkernel\nalice68.folder=Games\n', 'config.txt.bak')
o, rc = run('CARD DELETE old.txt')
chk(rc == 0 and 'old.txt' not in card, 'DELETE: %r' % o)
o, rc = run('CARD MKDIR "New Folder"')
chk('New Folder/' in card, 'MKDIR: %r' % o)
o, rc = run('CARD RENAME kernel8.img kernel8.old')
chk('kernel8.old' in card and 'kernel8.img' not in card, 'RENAME: %r' % o)
o, rc = run('CARD LIST HDF')
chk([l.split()[0] for l in o.splitlines()[1:]] == ['Prism.hdf', 'Spare.hdf'] and 'in use' in o.splitlines()[1], 'LIST HDF: %r' % o)
o, rc = run('CARD DELETE HDF/Prism.hdf')
chk(rc == 10 and 'in use' in o and 'HDF/Prism.hdf' in card, 'deleting a mounted hardfile: %r' % o)
o, rc = run('CARD LIST EXFAT')
chk(rc == 10 and 'not available' in o, 'LIST EXFAT without one: %r' % o)
o, rc = run('CARD BOOT Spare.hdf')
chk(b'hdf.boot=Spare' in card['cmdline.txt'] and b'hdf.boot=Prism' not in card['cmdline.txt'], 'boot from Spare.hdf: %r' % card['cmdline.txt'])
o, rc = run('CARD SET "wifi.psk=new pass 2"')
chk(b'wifi.psk="new pass 2"' in card['cmdline.txt'] and b'old secret' not in card['cmdline.txt'], 'the new password: %r' % card['cmdline.txt'])
o, rc = run('CARD CONFIG')
chk('wifi.psk=********' in o and 'new pass' not in o and '6   alice68.folder=Games' in o, 'CONFIG shown: %r' % o)
o, rc = run('CARD CMDLINE')
chk(o.strip().startswith('coherent_pool=1M') and 'wifi.psk=********' in o and 'new pass' not in o, 'CMDLINE shown: %r' % o)
line = o.strip()
o, rc = run('CARD CMDLINE "%s iso=off"' % line.replace('"', ''))
chk(b' iso=off' in card['cmdline.txt'] and b'wifi.psk="new pass 2"' in card['cmdline.txt'] and b'********' not in card['cmdline.txt'],
    'the line typed in kept with the real password: %r' % card['cmdline.txt'])
run('CARD CONFIG SET 6 alice68.folder=Games:Spiele')
run('CARD CONFIG ADD hdmi_force_hotplug=1')
run('CARD CONFIG DELETE 3')
chk(card['config.txt'] == b'# the Pi\n[pi4]\n[all]\ninitramfs kick32.rom followkernel\nalice68.folder=Games:Spiele\nhdmi_force_hotplug=1\n', 'config.txt lines %r' % card['config.txt'])
o, rc = run('CARD CONFIG DELETE 99')
chk(rc == 10, 'deleting a line that is not there: %r' % o)
# Bluetooth
o, rc = run('BT')
chk(rc == 0 and 'Bluetooth: off' in o, 'BT before it starts: %r' % o)
o, rc = run('BT SCAN')
chk(rc == 10 and 'not ready' in o, 'BT SCAN before it is ready: %r' % o)
bt_ready()
o, rc = run('BT LIST')
chk('Bluetooth: ready' in o and any('Slim Keys' in l and 'keyboard' in l and '16:15:14:13:12:11' in l and '-58 dBm' in l for l in o.splitlines()), 'BT LIST found: %r' % o)
o, rc = run('BT SCAN')
chk(struct.unpack('>I', board[BT + 32:BT + 36])[0] == 1 and struct.unpack('>I', board[BT + 36:BT + 40])[0] == 1, 'BT SCAN: %r' % o)
o, rc = run('BT PAIR 16:15:14:13:12:11')
chk(struct.unpack('>I', board[BT + 32:BT + 36])[0] == 2 and bytes(board[BT + 48:BT + 54]) == bytes([0x11, 0x12, 0x13, 0x14, 0x15, 0x16]), 'BT PAIR: %r' % o)
o, rc = run('BT PAIR 16:15')
chk(rc == 10 and 'address' in o, 'BT PAIR with half an address: %r' % o)
bt_paired()
o, rc = run('BT LIST')
chk(any('Slim Keys' in l and 'connected' in l for l in o.splitlines()) and any('EOS Mini' in l and 'game pad' in l for l in o.splitlines()) and
    any('LE Mouse' in l and 'LE' in l for l in o.splitlines()), 'BT LIST paired: %r' % o)
o, rc = run('BT KEEP')
c = card['cmdline.txt']
chk(b'bt.dev=161514131211:000102030405060708090a0b0c0d0e0f:1' in c, 'pairing kept in cmdline.txt: %r' % c)
chk(b'bt.dev=262524232221:202122232425262728292a2b2c2d2e2f:6:1234:0102030405060708:1:404142434445464748494a4b4c4d4e4f' in c, 'LE pairing kept: %r' % c)
chk(b'bt.dev=363534333231:303132333435363738393a3b3c3d3e3f:8' in c, 'game pad kept with kind 8: %r' % c)
chk(b' iso=off' in c and b'wifi.psk="new pass 2"' in c, 'KEEP kept the rest of cmdline.txt: %r' % c)
o, rc = run('BT FORGET 16:15:14:13:12:11')
chk(struct.unpack('>I', board[BT + 32:BT + 36])[0] == 3, 'BT FORGET: %r' % o)
o, rc = run('CARD RESTART')
chk(struct.unpack('>I', board[44:48])[0] == 0x52455354 and 'Restarting' in o, 'restart not asked on the board: %r' % o)
o, rc = run('CARD FROB')
chk(rc == 10 and 'Alice68 CARD' in o, 'unknown CARD word: %r' % o)
if fails:
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('CARD: partitions, mounted files, free space, listing; settings, Kickstart, boot hardfile, cmdline line and config.txt lines '
      'saved with .bak; password never shown; delete, new folder, rename, in-use refused; BT: list, scan, pair, keep, forget; restart')
print('ALL OK')
