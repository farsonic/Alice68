#!/usr/bin/env python3
"""Alice68-ShowConfig on a 68k emulator (machine68k), against a fake exec,
graphics, expansion and dos.library: an AGA machine with chip and fast memory,
Emu68's ROM board, the Alice68 USB and network cards (filled in as the kernel
would, at the offsets the C compiler gives for alice68_usbpt.h and alice_net.h)
and alice68hdf.device with a hardfile and a folder; then a machine without
any of Alice68's parts.

    harness_showconfig.py Alice68-ShowConfig
SPDX-License-Identifier: GPL-2.0-only"""
import os, struct, subprocess, sys, tempfile
import machine68k as M

HERE = os.path.dirname(os.path.abspath(__file__))
AMIGA = os.path.dirname(HERE)

# ---- structure offsets, from the 68k compiler (char arrays sized offset+1, read back with nm) ----
FIELDS = {
    'HdfBase': ['magic', 'nunits', 'unit', 'card_dev', 'cmd64', 'readonly', 'reads', 'read_errors', 'writes', 'write_errors', 'nfvols', 'fvol'],
    'HUnit': ['f', 'single', 'chosen'],
    'hdf_file': ['name', 'size', 'next'],
    'pt_hdr': ['magic', 'version', 'nports', 'nreqs', 'klog_wr', 'alog_wr', 'amiga_ver', 'port'],
    'pt_req': ['state'],
    'an_hdr': ['magic', 'version', 'nunits', 'kernel_ver', 'amiga_ver', 'fw_state', 'fw_count', 'trace'],
    'an_unit': ['kind', 'state', 'link', 'speed_mbps', 'mac', 'msg', 'ssid', 'bssid', 'rssi', 'channel', 'security', 'chip', 'chiprev', 'sdmhz',
                'fwver', 'rx_pkts', 'tx_pkts', 'rx_bytes', 'tx_bytes', 'rx_drop', 'tx_err', 'scan_count', 'rx_head', 'rx_tail', 'tx_head',
                'tx_tail', 'rx_on', 'factory_mac', 'link_adv', 'link_lpa', 'link_flags', 'link_downs', 'link_up_s'],
}
src = '#include <stddef.h>\n#include "amiga.h"\n#include "../third_party/Emu68/src/frf_usb/xhci/alice68_usbpt.h"\n' \
      '#include "../third_party/Emu68/src/alice/alice_net.h"\n#include "hdf/hdf_dev.h"\n'
for s, fs in FIELDS.items():
    tag = s if s in ('HdfBase', 'HUnit') else s
    for f in fs: src += 'char o_%s_%s[offsetof(struct %s, %s) + 1];\n' % (s, f, tag, f)
    src += 'char z_%s[sizeof(struct %s)];\n' % (s, tag)
with tempfile.TemporaryDirectory() as td:
    open(td + '/o.c', 'w').write(src)
    subprocess.run(['m68k-linux-gnu-gcc', '-m68020', '-fcommon', '-c', '-I', AMIGA, '-o', td + '/o.o', td + '/o.c'], check=True)
    nm = subprocess.run(['m68k-linux-gnu-nm', '-S', td + '/o.o'], capture_output=True, text=True, check=True).stdout
OFF = {}
for l in nm.splitlines():
    p = l.split()
    if len(p) == 4 and p[3][:2] in ('o_', 'z_'):
        OFF[p[3][2:] if p[3][0] == 'o' else p[3]] = int(p[1], 16) - (1 if p[3][0] == 'o' else 0)

R = M.Register
hunk = sys.argv[1]
fails = []

def run(args, alice68=True):
    mach = M.Machine(M.CPUType.M68020, 16384)
    cpu, mem, traps = mach.cpu, mach.mem, mach.traps
    SYSBASE, GFX, EXP, DOS = 0x8000, 0xB000, 0xC000, 0xD000
    LOAD, HEAP, ARGS, STACK = 0x20000, 0x200000, 0x1F000, 0x1F0000
    USB, NET = 0xE90000, 0xEA0000
    st = {'done': False, 'exc': None, 'out': [], 'unknown': []}
    heap = {'p': HEAP}
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
    def t_unk(op, pc): st['unknown'].append(pc)
    UNK = traps.setup(t_unk, auto_rts=True)
    for b in (SYSBASE, GFX, EXP, DOS):
        for off in range(6, 1000, 6): mem.w16(b - off, 0xA000 | UNK)
    def alloc(n):
        a = (heap['p'] + 7) & ~7; heap['p'] = a + n; mem.w_block(a, bytes(n)); return a
    def cstr(s):
        a = alloc(len(s) + 1); mem.w_block(a, s.encode('latin1') + b'\0'); return a
    def newlist(l): mem.w32(l, l + 4); mem.w32(l + 4, 0); mem.w32(l + 8, l)
    def addtail(l, n):
        pred = mem.r32(l + 8); mem.w32(n, l + 4); mem.w32(n + 4, pred); mem.w32(pred, n); mem.w32(l + 8, n)
    def lib(name, ver, rev, size=40, at=None):
        a = at or alloc(size); mem.w32(a + 10, cstr(name)); mem.w16(a + 20, ver); mem.w16(a + 22, rev); mem.w16(a + 32, 1); return a
    # exec
    lib('exec.library', 40, 68, at=SYSBASE)
    mem.w16(SYSBASE + 296, 0x7f)                                   # 68010..68040, 68881/2, FPU40
    mem.w8(SYSBASE + 530, 50); mem.w8(SYSBASE + 531, 50); mem.w32(SYSBASE + 568, 709379)
    for off in (322, 336, 350, 378): newlist(SYSBASE + off)
    for name, attr, lo, hi, fr in (('chip memory', 0x703, 0x400, 0x200000, 0x150000), ('expansion memory', 0x105, 0x40000000, 0x48000000, 0x7000000)):
        h = alloc(32); mem.w32(h + 10, cstr(name)); mem.w16(h + 14, attr); mem.w32(h + 20, lo); mem.w32(h + 24, hi); mem.w32(h + 28, fr)
        addtail(SYSBASE + 322, h)
    lib('graphics.library', 40, 42, at=GFX); mem.w16(GFX + 206, 4); mem.w8(GFX + 236, 0x0f)
    lib('expansion.library', 40, 2, at=EXP)
    lib('dos.library', 40, 3, at=DOS)
    for a in (SYSBASE, GFX, EXP, DOS): addtail(SYSBASE + 378, a)
    addtail(SYSBASE + 378, lib('icon.library', 44, 1))
    addtail(SYSBASE + 350, lib('timer.device', 40, 1))
    r = alloc(20); mem.w32(r + 10, cstr('FileSystem.resource')); addtail(SYSBASE + 336, r)
    libs = {'graphics.library': GFX, 'expansion.library': EXP, 'dos.library': DOS}
    lvo(SYSBASE, -552, lambda o, p: setd0(libs.get(mem.r_cstr(d(R.A1)), 0)))
    for off in (-414, -132, -138, -210): lvo(SYSBASE, off, lambda o, p: None)
    lvo(SYSBASE, -198, lambda o, p: setd0(alloc(d(R.D0))))
    def availmem(o, p):
        f = d(R.D1)
        chip = f & 2
        setd0((0x1FFC00 if chip else 0x8000000) if f & 0x80000 else (0x100000 if chip else 0x6000000) if f & 0x20000 else (0x150000 if chip else 0x7000000))
    lvo(SYSBASE, -216, availmem)
    def findname(o, p):
        l, want = d(R.A0), mem.r_cstr(d(R.A1))
        n = mem.r32(l)
        while mem.r32(n):
            nm_ = mem.r32(n + 10)
            if nm_ and mem.r_cstr(nm_) == want: setd0(n); return
            n = mem.r32(n)
        setd0(0)
    lvo(SYSBASE, -276, findname)
    lvo(DOS, -60, lambda o, p: setd0(1))
    lvo(DOS, -48, lambda o, p: (st['out'].append(bytes(mem.r_block(d(R.D2), d(R.D3))).decode('latin1')), setd0(d(R.D3))))
    # boards
    cds = []
    def board(z3, man, prod, serial, addr, size, typ_extra=0, driver=None):
        c = alloc(68)
        mem.w8(c + 14, 8)                                          # CDF_PROCESSED
        mem.w8(c + 16, (0x80 if z3 else 0xc0) | typ_extra); mem.w8(c + 17, prod); mem.w16(c + 20, man); mem.w32(c + 22, serial)
        mem.w32(c + 32, addr); mem.w32(c + 36, size)
        if driver: mem.w32(c + 44, lib(driver, 1, 0))
        cds.append(c)
    board(False, 0x0202, 3, 0, 0xE80000 if not alice68 else 0xEC0000, 0x10000, 0x10, 'scsi.device')
    if alice68:
        board(False, 0x6d73, 2, 0, 0xE00000 - 0x10000 if False else 0xEE0000, 0x10000, 0x10)
        board(False, 0x6d73, 0x6a, 0x55534250, USB, 0x10000, 0, 'alice68-usb.device')
        board(False, 0x6d73, 0x6b, 0, NET, 0x20000, 0, 'alice68net.device')
        board(True, 0x6d73, 0x11, 1, 0x40000000, 0x8000000, 0x20)
    def findcd(o, p):
        old = d(R.A0)
        i = cds.index(old) + 1 if old in cds else 0
        setd0(cds[i] if i < len(cds) else 0)
    lvo(EXP, -72, findcd)
    if alice68:
        # the USB card, as the kernel fills it
        O = OFF
        mem.w32(USB + O['pt_hdr_magic'], 0x45415550); mem.w32(USB + O['pt_hdr_version'], 2)
        mem.w32(USB + O['pt_hdr_nports'], 8); mem.w32(USB + O['pt_hdr_nreqs'], 32)
        mem.w32(USB + O['pt_hdr_amiga_ver'], 0x10000); mem.w32(USB + O['pt_hdr_alog_wr'], 777)
        port = USB + O['pt_hdr_port']
        mem.w32(port + 16 * 2, 1); mem.w32(port + 16 * 2 + 4, 3); mem.w32(port + 16 * 2 + 12, 0x0bda8156)   # port 3: RTL8156
        for i, s_ in enumerate((2, 2, 3)): mem.w32(USB + 0x200 + 64 * i + O['pt_req_state'], s_)
        log = b''.join(b'[FRF-XHCI] line %d of the kernel log\n' % i for i in range(300))
        for i, ch in enumerate(log): mem.w8(USB + 0x8000 + i % 0x8000, ch)
        mem.w32(USB + O['pt_hdr_klog_wr'], len(log))
        # the network card
        mem.w32(NET + O['an_hdr_magic'], 0x4136384e); mem.w32(NET + O['an_hdr_version'], 1); mem.w32(NET + O['an_hdr_nunits'], 2)
        mem.w32(NET + O['an_hdr_kernel_ver'], 1); mem.w32(NET + O['an_hdr_amiga_ver'], (2 << 16) | 7)
        mem.w32(NET + O['an_hdr_fw_state'], 2); mem.w32(NET + O['an_hdr_fw_count'], 3)
        def unit(u, **k):
            b = NET + 0x400 + u * 0x800
            for f, v in k.items():
                if isinstance(v, bytes): mem.w_block(b + O['an_unit_' + f], v)
                else: mem.w32(b + O['an_unit_' + f], v & 0xffffffff)
        unit(0, kind=1, state=5, link=1, speed_mbps=433, mac=bytes([0, 0x80, 0x10, 1, 2, 3]), factory_mac=bytes([0xdc, 0xa6, 0x32, 1, 2, 3]),
             ssid=b'Example-Net\0', bssid=bytes([2, 0x11, 0x22, 0x33, 0x44, 0x55]), rssi=-52, channel=36, security=0x08,
             chip=0x4345, chiprev=6, sdmhz=25, fwver=b'7.45.241 (Example)\0', rx_pkts=1234, tx_pkts=987, rx_bytes=5 << 20, tx_bytes=1 << 20,
             scan_count=7, rx_head=10, rx_tail=8, tx_head=5, tx_tail=5, rx_on=1, msg=b'connected to Example-Net\0')
        unit(1, kind=2, state=5, link=1, speed_mbps=1000, mac=bytes([0, 0x80, 0x10, 4, 5, 6]), link_flags=0x0f, link_adv=0x3f | 0x40,
             link_lpa=0x2f, link_downs=1, link_up_s=12, rx_pkts=55, tx_pkts=44, rx_bytes=70000, tx_bytes=9000, rx_drop=2, rx_on=1)
        # alice68hdf.device
        db = alloc(OFF['z_HdfBase'])
        lib('alice68hdf.device', 0, 5, at=db); addtail(SYSBASE + 350, db)
        mem.w32(db + O['HdfBase_magic'], 0x41363848); mem.w32(db + O['HdfBase_nunits'], 1)
        hu = alloc(OFF['z_HUnit'])
        f = hu + O['HUnit_f']
        mem.w_block(f + O['hdf_file_name'], b'Workbench.hdf\0'); mem.w32(f + O['hdf_file_size'], 500 << 20); mem.w32(f + O['hdf_file_next'], 2)
        mem.w32(hu + O['HUnit_single'], 1)
        mem.w32(db + O['HdfBase_unit'], hu)
        mem.w32(db + O['HdfBase_card_dev'], cstr('brcm-emmc.device')); mem.w32(db + O['HdfBase_cmd64'], 0xc000)
        mem.w32(db + O['HdfBase_reads'], 4321); mem.w32(db + O['HdfBase_writes'], 12)
        fv = alloc(176); mem.w_block(fv + 4, b'ALICE68\0'); mem.w_block(fv + 132, b'ALICE68\0'); mem.w_block(fv + 164, b'SDF0\0')
        mem.w32(db + O['HdfBase_nfvols'], 1); mem.w32(db + O['HdfBase_fvol'], fv)
    # load and run
    h = open(hunk, 'rb').read(); w = lambda o: struct.unpack('>I', h[o:o + 4])[0]
    n = w(28) * 4; code = bytearray(h[32:32 + n]); p = 32 + n
    if w(p) == 0x3ec:
        for i in range(w(p + 4)):
            r_ = w(p + 12 + 4 * i); v = struct.unpack('>I', code[r_:r_ + 4])[0]; code[r_:r_ + 4] = struct.pack('>I', v + LOAD)
    mem.w_block(LOAD, bytes(code))
    mem.w_block(ARGS, args.encode() + b'\n\0')
    mem.w32(STACK, 0x1000)
    cpu.w_reg(R.A7, STACK); cpu.w_reg(R.A0, ARGS); cpu.w_reg(R.D0, len(args) + 1); cpu.w_pc(LOAD); cpu.w_sr(0)
    for _ in range(3000):
        if st['done']: break
        cpu.execute(100000)
    if st['exc']: fails.append('%r: CPU exception %d at $%x' % (args, st['exc'][0], st['exc'][1]))
    if not st['done']: fails.append('%r: did not return' % args)
    if st['unknown']: fails.append('%r: library calls the harness does not have, at %s' % (args, ['$%x' % x for x in st['unknown']]))
    return ''.join(st['out'])

def want(name, text, has, hasnt=()):
    for s in has:
        if s not in text: fails.append('%s: no "%s"' % (name, s))
    for s in hasnt:
        if s in text: fails.append('%s: has "%s"' % (name, s))

t = run('')
print(t)
want('default', t, [
    'Processor   68040, FPU: the CPU\'s own  (Emu68', 'Kickstart   exec 40.68 (Kickstart 3.1)', 'Chipset     AGA  (Alice, Lisa)', 'PAL display',
    'E clock 709379 Hz, vertical blank 50 Hz',
    'chip memory', '$00000400-$001fffff', 'CHIP PUBLIC LOCAL 24BITDMA', 'expansion memory', 'FAST PUBLIC LOCAL',
    'Chip: 2047 KB total, 1344 KB free, largest 1024 KB', 'Fast: 128 MB total, 112 MB free, largest 96 MB',
    'EXPANSION BOARDS (5)', 'manufacturer 514 ($0202) Commodore, product 3', 'boot ROM', 'driver: scsi.device',
    'Emu68 ROM board', 'Alice68 USB card', 'serial $55534250 "USBP"', 'driver: alice68-usb.device', 'Alice68 network card',
    'Zorro III  $40000000  128 MB', 'Zorro III RAM (EMU68AGA)', 'memory (added to the free memory)',
    'USB card at $00e90000', 'layout 2, 8 ports, 32 request slots; alice68-usb.device: 1.0', 'port 3: 0bda:8156, high speed (480 Mbit/s)',
    'requests: 29 free, 0 waiting for the kernel, 2 in progress, 1 finished', 'driver 777 bytes',
    'Network card at $00ea0000', 'kernel side 1, 2 units; alice68net.device: 2.7', 'WiFi firmware: taken by the kernel (3 files)',
    'unit 0 WiFi: connected, link up, 433 Mbit/s', 'MAC 00:80:10:01:02:03 (hardware\'s own dc:a6:32:01:02:03)',
    'network "Example-Net", access point 02:11:22:33:44:55, channel 36, signal -52 dBm, WPA2', 'chip 4345 rev 6, SDIO 25 MHz, firmware 7.45.241 (Example)',
    'in:  1234 frames, 5120 KB, 0 dropped', 'queues: 2 of 16 receive slots waiting for the stack, 0 of 12 send slots',
    'last: connected to Example-Net',
    'unit 1 Ethernet: connected, link up, 1000 Mbit/s', 'full duplex, autonegotiated, flow control both ways',
    'offered here: 10 half, 10 full, 100 half, 100 full, 1000 half, 1000 full, pause', 'offered by the other side: 10 half, 10 full, 100 half, 100 full, 1000 full',
    'link went down once; up since 12 s after power-on', '2 dropped',
    'alice68hdf.device 0.5 (on Emu68\'s ROM board): card brcm-emmc.device (64-bit), read-write',
    '1 hardfile unit, 4321 card reads, 12 writes', 'unit 0: Workbench.hdf, 500 MB in 2 pieces, single partition', 'folder ALICE68: volume ALICE68: (SDF0:)',
], ['LIBRARIES', 'KERNEL LOG'])
a = run('ALL')
want('ALL', a, ['LIBRARIES (5)', 'graphics.library', ' 40.42', 'icon.library', 'DEVICES (2)', 'alice68hdf.device', 'timer.device',
                'RESOURCES (1)', 'FileSystem.resource', 'KERNEL LOG (the last 4 KB)', 'line 299 of the kernel log'], ['line 100 of'])
m = run('boards')
want('BOARDS only', m, ['EXPANSION BOARDS (5)'], ['SYSTEM', 'MEMORY', 'ALICE68\n'])
u = run('?')
want('?', u, ['Alice68-ShowConfig [SYS] [MEM] [BOARDS] [CARDS] [LIBS] [LOG] [ALL]'], ['SYSTEM'])
x = run('', alice68=False)
want('not Alice68', x, ['EXPANSION BOARDS (1)', 'no Alice68 cards: not an Alice68 kernel', 'alice68hdf.device: not running'], ['Emu68:'])
xl = run('LOG', alice68=False)
want('not Alice68, LOG', xl, ['not available: it is kept on the USB card'])
for f in fails: print('FAIL:', f)
print('ALL OK' if not fails else '%d FAILED' % len(fails))
sys.exit(1 if fails else 0)
