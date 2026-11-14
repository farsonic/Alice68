#!/usr/bin/env python3
"""Alice68-Card (the SD card manager, GadTools) on a 68k emulator (machine68k) with fake
intuition, gadtools, graphics, dos, exec, expansion and alice68hdf.device's card service
(a card held in Python). A scripted user goes through every page: changes settings and
saves, picks a Kickstart, edits config.txt, deletes, renames and makes folders, restarts
the Pi, closes. Checks what reached the card service and the network board, and that
every allocation was given back.

    harness_card.py amiga/out/Alice68-Card
SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys
import machine68k as M
R = M.Register
fails = []
mach = M.Machine(M.CPUType.M68020, 8192)
cpu, mem, traps = mach.cpu, mach.mem, mach.traps
SYS, DOS, INT, GT, GFX, EXP, LOAD, HEAP, ARGS, STACK = 0x8000, 0xA000, 0xB000, 0xC000, 0xD000, 0xE000, 0x20000, 0x200000, 0x3F000, 0x80000
st = {'done': False, 'exc': None, 'heap': HEAP, 'out': [], 'gadgets': [], 'step': 0, 'allocs': {}, 'cs': []}
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
libs = {'dos.library': DOS, 'intuition.library': INT, 'gadtools.library': GT, 'graphics.library': GFX, 'expansion.library': EXP}
lvo(SYS, -552, lambda o, p: setd0(libs.get(mem.r_cstr(d(R.A1)), 0)))
lvo(SYS, -414, lambda o, p: None)
ME = alloc(232); mem.w32(ME + 172, 0x1234)
lvo(SYS, -294, lambda o, p: setd0(ME))
def allocmem(o, p):
    a = alloc(d(R.D0)); st['allocs'][a] = d(R.D0); setd0(a)
def freemem(o, p):
    a, n = d(R.A1), d(R.D0)
    if st['allocs'].get(a) != n: fails.append('FreeMem(%x, %d): allocated %s' % (a, n, st['allocs'].get(a)))
    st['allocs'].pop(a, None)
lvo(SYS, -198, allocmem); lvo(SYS, -210, freemem)
for off in (-132, -138): lvo(SYS, off, lambda o, p: None)
def newport(o, p): a = alloc(34); mem.w8(a + 15, 20); setd0(a)
lvo(SYS, -666, newport); lvo(SYS, -672, lambda o, p: None)
lvo(SYS, -654, lambda o, p: setd0(alloc(d(R.D0)))); lvo(SYS, -660, lambda o, p: None)
opened = []
def opendev(o, p):
    name, unit, io = mem.r_cstr(d(R.A0)), d(R.D0), d(R.A1)
    ok = name == 'alice68hdf.device' and unit == 0x41363843
    if ok: mem.w32(io + 20, 0x6666); opened.append(io)
    setd0(0 if ok else 0xffffffff)
lvo(SYS, -444, opendev)
lvo(SYS, -450, lambda o, p: opened.remove(d(R.A1)) if d(R.A1) in opened else fails.append('CloseDevice of something not open'))
lvo(SYS, -372, lambda o, p: setd0(0))
# ---- the card, as alice68hdf.device's card service sees it -------------------------------------
card = {
    'cmdline.txt': b'coherent_pool=1M aga.look=0 aga.scale=1 eth=off hdf.boot=Prism wifi.ssid="Home Net" net.trace=1\n',
    'config.txt': b'# the Pi\n[pi4]\narm_64bit=1\n[all]\ninitramfs kick31.rom followkernel\nalice68.folder=Games\n',
    'kick31.rom': bytes(524288), 'kick32.rom': bytes(524288), 'kernel8.img': bytes(5000), 'old.txt': b'old\n',
    'HDF/': None, 'HDF/Prism.hdf': bytes(4096), 'HDF/Spare.hdf': bytes(2048), 'ISO/': None,
}
INUSE = {'HDF/Prism.hdf'}
def ents(folder):
    pre = folder + '/' if folder else ''
    out = []
    for k in card:
        if not k.startswith(pre): continue
        rest = k[len(pre):]
        if not rest or ('/' in rest.rstrip('/')): continue
        out.append(rest)
    return out
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
        st['cs'].append(('INFO', off))
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
        st['cs'].append(('LIST', path))
    else:
        vol, path, path2, ln, mx = mem.r32(data), mem.r_cstr(data + 4), mem.r_cstr(data + 260), mem.r32(data + 516), mem.r32(data + 520)
        name = {11: 'READ', 12: 'WRITE', 13: 'DELETE', 14: 'RENAME', 15: 'MKDIR'}[cmd]
        st['cs'].append((name, path) + ((path2,) if cmd == 14 else ()) + ((bytes(mem.r_block(data + 524, ln)),) if cmd == 12 else ()))
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
# the network board
BOARD = alloc(0x400); mem.w32(BOARD, 0x4136384e)
CD = alloc(48); mem.w32(CD + 32, BOARD)
lvo(EXP, -72, lambda o, p: setd0(CD if (d(R.D0), d(R.D1)) == (0x6d73, 0x6b) else 0))
# dos
lvo(DOS, -60, lambda o, p: setd0(1))
def dwrite(o, p): st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')); setd0(d(R.D3))
lvo(DOS, -48, dwrite)
# intuition / gadtools / graphics
SCR = alloc(64); mem.w8(SCR + 35, 4); mem.w8(SCR + 36, 4); FONT = alloc(8); mem.w16(FONT + 4, 8); mem.w32(SCR + 40, FONT)
lvo(INT, -510, lambda o, p: setd0(SCR)); lvo(INT, -516, lambda o, p: None)
WIN = alloc(128); UPORT = alloc(34); mem.w8(UPORT + 15, 21); mem.w32(WIN + 86, UPORT); mem.w32(WIN + 50, alloc(100))
def openwin(o, p):
    t = d(R.A1); tags = {}
    while mem.r32(t): tags[mem.r32(t)] = mem.r32(t + 4); t += 8
    st['wintags'] = tags; setd0(WIN)
lvo(INT, -606, openwin); lvo(INT, -72, lambda o, p: st.__setitem__('closed', True))
attached = []
lvo(INT, -438, lambda o, p: (attached.append(d(R.A1)), setd0(0)))
def remglist(o, p):
    if d(R.A1) not in attached: fails.append('RemoveGList of a list not in the window')
    else: attached.remove(d(R.A1))
    setd0(0)
lvo(INT, -444, remglist)
lvo(INT, -432, lambda o, p: None)
asks = []
def easy(o, p):
    es = d(R.A1); asks.append(mem.r_cstr(mem.r32(es + 12))); setd0(1)
lvo(INT, -588, easy)
lvo(INT, -102, lambda o, p: setd0(1))
contexts = []
def createctx(o, p):
    c = alloc(64); mem.w32(d(R.A0), c); contexts.append(c); setd0(c)
lvo(GT, -114, createctx)
gstr, gcur, glab, gtext = {}, {}, {}, {}
def tagsof(t):
    out = {}
    while t and mem.r32(t): out[mem.r32(t)] = mem.r32(t + 4); t += 8
    return out
def creategad(o, p):
    kind, ng, t = d(R.D0), d(R.A1), tagsof(d(R.A2))
    gid = mem.r16(ng + 16); g = alloc(64); mem.w16(g + 38, gid)
    if kind == 12:
        si = alloc(36); buf = alloc(1024); mem.w32(si, buf); mem.w32(g + 34, si); gstr[gid] = buf
        if t.get(0x8008002d): mem.w_block(buf, mem.r_cstr(t[0x8008002d]).encode('latin1') + b'\0')
    if kind == 7:
        labs, a = [], t.get(0x8008000e, 0)
        while a and mem.r32(a): labs.append(mem.r_cstr(mem.r32(a))); a += 4
        glab[gid] = labs; gcur[gid] = t.get(0x8008000f, 0)
    st['gadgets'].append((kind, gid, g, mem.r_cstr(mem.r32(ng + 8)) if mem.r32(ng + 8) else '')); setd0(g)
lvo(GT, -30, creategad); lvo(GT, -36, lambda o, p: None)
lists = {}
def setattrs(o, p):
    g, t = d(R.A0), tagsof(d(R.A3))
    gid = mem.r16(g + 38)
    if 0x8008000b in t: gtext[gid] = mem.r_cstr(t[0x8008000b]) if t[0x8008000b] else ''
    if 0x80080006 in t and t[0x80080006] not in (0, 0xffffffff):
        names, n = [], mem.r32(t[0x80080006])
        while mem.r32(n): names.append(mem.r_cstr(mem.r32(n + 10))); n = mem.r32(n)
        lists[gid] = names
    if 0x8008002d in t and gid in gstr: mem.w_block(gstr[gid], mem.r_cstr(t[0x8008002d]).encode('latin1') + b'\0')
    if 0x8008000f in t: gcur[gid] = t[0x8008000f]
    if 0x8008000e in t:
        labs, a = [], t[0x8008000e]
        while a and mem.r32(a): labs.append(mem.r_cstr(mem.r32(a))); a += 4
        glab[gid] = labs
lvo(GT, -42, setattrs)
script = []
def gtget(o, p):
    if d(R.A0) != UPORT or not script: setd0(0); return
    cls, code, gid = script.pop(0)
    m = alloc(48); mem.w32(m + 20, cls); mem.w16(m + 24, code)
    if gid: mem.w32(m + 28, [x[2] for x in st['gadgets'] if x[1] == gid][0])
    setd0(m)
lvo(GT, -72, gtget)
for off in (-78, -84, -90, -96, -120, -132): lvo(GT, off, lambda o, p: None)
lvo(GT, -126, lambda o, p: setd0(alloc(16)))
for off in (-342, -306): lvo(GFX, off, lambda o, p: None)

# ---- what the user does, one event per turn of the loop -------------------------------------------
G = dict(TAB0=1, TAB1=2, TAB2=3, TAB3=4, SAVE=5, RESTART=6, STATUS=7, CARD=8, PARTS=9, EXFAT=10, USED=11, FREE=12, VOL=13, FILTER=14,
         PARENT=15, PATH=16, FILES=17, OPEN=18, BOOT=19, KICK=20, DELETE=21, RENAME=22, MKDIR=23, NAME=24, FINFO=25, KS=26, REVERT=27,
         HELP0=28, LINES=31, LINE=32, ADDL=33, DELL=34, CMD=35, TNOTE=36, OPT0=37)
UP, CLOSE = 0x40, 0x200
def typ(gid, s): mem.w_block(gstr[gid], s.encode('latin1') + b'\0')
def ev(gid, code=0, cls=UP): return lambda: script.append((cls, code, gid))
def idx(gid, name): return [i for i, x in enumerate(lists[gid]) if x == name or (x.split() and x.split()[0] == name)][0]
def pick(gid, name): return lambda: script.append((UP, idx(gid, name), gid))
checks = {}
def note(k): return lambda: checks.__setitem__(k, (dict(card), list(st['cs']), dict(gtext), {k_: list(v) for k_, v in lists.items()}))
steps = [
    note('start'),
    ev(G['FREE']), note('free'),
    ev(G['TAB2']),
    ev(G['OPT0'] + 1, 4),                                  # Scaling: 16:9
    lambda: (typ(G['OPT0'] + 10, 'gb'), script.append((UP, 0, G['OPT0'] + 10))),   # Country, typed in lower case
    ev(G['KS'], 1),                                        # Kickstart: kick32.rom
    ev(G['SAVE']), note('saved'),
    ev(G['TAB1']),
    pick(G['FILES'], 'old.txt'), ev(G['DELETE']), note('deleted'),
    lambda: (typ(G['NAME'], 'New Folder'), script.append((UP, 0, G['MKDIR']))), note('mkdir'),
    pick(G['FILES'], 'kernel8.img'), lambda: (typ(G['NAME'], 'kernel8.old'), script.append((UP, 0, G['RENAME']))), note('renamed'),
    pick(G['FILES'], 'HDF/'), ev(G['OPEN']), note('hdf'),
    pick(G['FILES'], 'Prism.hdf'), ev(G['DELETE']), note('inuse'),
    pick(G['FILES'], 'Spare.hdf'), ev(G['BOOT']), ev(G['PARENT']),
    ev(G['TAB3']),
    pick(G['LINES'], 'alice68.folder=Games'), lambda: (typ(G['LINE'], 'alice68.folder=Games:Spiele'), script.append((UP, 0, G['LINE']))),
    lambda: (typ(G['LINE'], 'hdmi_force_hotplug=1'), script.append((UP, 0, G['ADDL']))),
    pick(G['LINES'], 'arm_64bit=1'), ev(G['DELL']), note('lines'),
    ev(G['SAVE']), note('saved2'),
    ev(G['RESTART']), note('restart'),
    ev(0, 0, CLOSE),
]
def wait2(o, p):
    st['step'] += 1
    if st['step'] > 200: fails.append('the event loop never ended'); st['done'] = True; cpu.end(); return
    while steps:
        s = steps.pop(0); s()
        if script: break
    setd0(d(R.D0) & ~0x1000)
lvo(SYS, -318, wait2)

# ---- the program ------------------------------------------------------------------------------
h = open(sys.argv[1], 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
if w(p) == 0x3ec:
    for i in range(w(p + 4)):
        r = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r:r + 4])[0]; code[r:r + 4] = struct.pack('>I', v + LOAD)
mem.w_block(LOAD, bytes(code))
mem.w_block(ARGS, b'\n\0'); mem.w32(STACK, 0x1000)
cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, 1); cpu.w_pc(LOAD); cpu.w_sr(0)
for _ in range(20000):
    if st['done']: break
    cpu.execute(100000)
if st['exc']: fails.append('CPU exception %d at $%x (offset $%x)' % (st['exc'][0], st['exc'][1], st['exc'][1] - LOAD))
if not st['done']: fails.append('did not return')
def chk(c, m):
    if not c: fails.append(m)
# what the window was made of
kinds = sorted(set(k for k, *_ in st['gadgets']))
chk(kinds == [1, 4, 7, 12, 13], 'gadget kinds %s' % kinds)
labels = [t for k, i, g, t in st['gadgets'] if i >= G['OPT0']]
chk(labels[:4] == ['Picture', 'Scaling', 'Boot log panels', 'Hardfiles'] and 'Country' in labels and len(labels) == 15, 'settings gadgets %s' % labels)
chk(st.get('wintags', {}).get(0x80000076) == 632, 'window width %s' % st.get('wintags'))
if 'start' in checks:
    c, calls, texts, lst = checks['start']
    chk(('READ', 'cmdline.txt') in calls and ('READ', 'config.txt') in calls and ('INFO', 0) in calls, 'start: %s' % calls)
    chk(any('ALICE68' in x and 'FAT32' in x and 'Alice68\'s files' in x for x in lst.get(G['PARTS'], [])), 'partitions %s' % lst.get(G['PARTS']))
    chk(any('Emu68 RDB disk' in x for x in lst.get(G['PARTS'], [])), 'the Amiga partition %s' % lst.get(G['PARTS']))
    chk(lst.get(G['USED']) and 'Prism.hdf' in lst[G['USED']][0], 'mounted %s' % lst.get(G['USED']))
    chk([x.split()[0] for x in lst.get(G['FILES'], [])][:3] == ['HDF/', 'ISO/', 'cmdline.txt'], 'files sorted, folders first: %s' % lst.get(G['FILES']))
    chk(texts.get(G['CARD'], '').startswith('brcm-emmc.device unit 0, read-write'), 'card line %r' % texts.get(G['CARD']))
if 'free' in checks:
    chk(('INFO', 1) in checks['free'][1] and any('free' in x for x in checks['free'][3].get(G['PARTS'], [])), 'free space not counted')
if 'saved' in checks:
    c = checks['saved'][0]
    chk(c.get('cmdline.txt') == b'coherent_pool=1M aga.look=0 eth=off hdf.boot=Prism wifi.ssid="Home Net" net.trace=1 aga.scale=4 wifi.country=GB\n',
        'cmdline.txt saved as %r' % c.get('cmdline.txt'))
    chk(c.get('cmdline.txt.bak', b'').startswith(b'coherent_pool=1M aga.look=0 aga.scale=1'), 'cmdline.txt.bak %r' % c.get('cmdline.txt.bak'))
    chk(b'initramfs kick32.rom followkernel\n' in c.get('config.txt', b'') and b'kick31' not in c['config.txt'], 'config.txt Kickstart: %r' % c.get('config.txt'))
    chk(c.get('config.txt.bak') == b'# the Pi\n[pi4]\narm_64bit=1\n[all]\ninitramfs kick31.rom followkernel\nalice68.folder=Games\n', 'config.txt.bak')
if 'deleted' in checks:
    chk('old.txt' not in checks['deleted'][0] and asks and 'old.txt' in asks[0], 'delete: asks %s' % asks)
chk('mkdir' in checks and 'New Folder/' in checks['mkdir'][0], 'new folder')
chk('renamed' in checks and 'kernel8.old' in checks['renamed'][0] and 'kernel8.img' not in checks['renamed'][0], 'rename')
chk('hdf' in checks and [x.split()[0] for x in checks['hdf'][3].get(G['FILES'], [])] == ['Prism.hdf', 'Spare.hdf'] and 'in use' in checks['hdf'][3][G['FILES']][0],
    'HDF folder: %s' % (checks.get('hdf', [0, 0, 0, {}])[3].get(G['FILES'])))
chk('inuse' in checks and 'HDF/Prism.hdf' in checks['inuse'][0] and 'in use' in checks['inuse'][2].get(G['STATUS'], ''), 'deleting a mounted hardfile: %r' % (checks.get('inuse', [0, 0, {}])[2].get(G['STATUS'])))
if 'lines' in checks:
    ls = checks['lines'][3].get(G['LINES'], [])
    chk('alice68.folder=Games:Spiele' in ls and 'hdmi_force_hotplug=1' in ls and 'arm_64bit=1' not in ls, 'config.txt lines %s' % ls)
if 'saved2' in checks:
    c = checks['saved2'][0]
    chk(c.get('config.txt') == b'# the Pi\n[pi4]\n[all]\ninitramfs kick32.rom followkernel\nalice68.folder=Games:Spiele\nhdmi_force_hotplug=1\n', 'config.txt %r' % c.get('config.txt'))
    chk(b'hdf.boot=Spare' in c.get('cmdline.txt', b'') and b'hdf.boot=Prism' not in c['cmdline.txt'], 'boot from Spare.hdf: %r' % c.get('cmdline.txt'))
missing = [k for k in ('start', 'free', 'saved', 'deleted', 'mkdir', 'renamed', 'hdf', 'inuse', 'lines', 'saved2', 'restart') if k not in checks]
chk(not missing, 'steps never reached: %s' % missing)
chk(mem.r32(BOARD + 44) == 0x52455354, 'restart not asked on the board (%x)' % mem.r32(BOARD + 44))
chk(st.get('closed'), 'window not closed')
chk(not opened, 'the device left open')
chk(not st['allocs'], 'memory not given back: %s' % st['allocs'])
chk(not attached, 'a page\'s gadgets left in the window at the end')
if fails:
    print(''.join(st['out'])); print('card service calls:', st['cs'][-12:])
    print('\n'.join('FAIL: ' + f for f in fails)); sys.exit(1)
print('%d gadgets; partitions, mounted files, free space; settings and Kickstart saved (with .bak); delete (asked), new folder, '
      'rename, in-use refused, boot from a hardfile; config.txt lines edited; restart asked; window closed, memory back' % len(st['gadgets']))
print('ALL OK')
