#!/usr/bin/env python3
"""Draw docs/images/alice68-cards.svg: Alice68's virtual Zorro II cards - the Pi 4
hardware, the kernel on CPU1, the two cards with their real memory maps
(alice68_usbpt.h, alice_net.h) and the AmigaOS drivers on them.
Original drawing.   python3 docs/images/mkcards.py
SPDX-License-Identifier: GPL-2.0-only"""
import os

W, H = 1800, 1360
o = []
def a(s): o.append(s)
def esc(t): return t.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
def text(x, y, t, size=14, fill='#1d2433', anchor='start', weight='normal', family='sans', extra=''):
    fam = "'DejaVu Sans Mono', Menlo, Consolas, monospace" if family == 'mono' else "'Segoe UI', Helvetica, Arial, sans-serif"
    a('<text x="%g" y="%g" font-family="%s" font-size="%g" fill="%s" text-anchor="%s" font-weight="%s" %s>%s</text>'
      % (x, y, fam, size, fill, anchor, weight, extra, esc(t)))
def lines(x, y, ts, size=14, lh=None, **k):
    lh = lh or size * 1.3
    for i, t in enumerate(ts): text(x, y + i * lh, t, size, **k)
def rect(x, y, w, h, fill, stroke='none', sw=1, rx=0, extra=''):
    a('<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="%s" stroke="%s" stroke-width="%g" %s/>' % (x, y, w, h, rx, fill, stroke, sw, extra))
def line(x1, y1, x2, y2, stroke, sw=2, extra=''):
    a('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g" %s/>' % (x1, y1, x2, y2, stroke, sw, extra))
def path(d, stroke, sw=2, fill='none', extra=''):
    a('<path d="%s" stroke="%s" stroke-width="%g" fill="%s" %s/>' % (d, stroke, sw, fill, extra))
def badge(x, y, n, col='#e8590c'):
    a('<circle cx="%g" cy="%g" r="13" fill="%s" stroke="#fff" stroke-width="2.5" filter="url(#sh)"/>' % (x, y, col))
    text(x, y + 5, str(n), 15, '#fff', 'middle', 'bold')

ORANGE, RED, BLUE = '#e8590c', '#d6336c', '#1c7ed6'

a('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (W, H, W, H))
a('''<defs>
 <linearGradient id="pcb" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2f8f4e"/><stop offset="1" stop-color="#1b6236"/></linearGradient>
 <linearGradient id="pcb2" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#1f6f8b"/><stop offset="1" stop-color="#134a5e"/></linearGradient>
 <linearGradient id="pcb3" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#6b4fa0"/><stop offset="1" stop-color="#43306a"/></linearGradient>
 <linearGradient id="metal" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#f1f3f5"/><stop offset=".5" stop-color="#ced4da"/><stop offset="1" stop-color="#adb5bd"/></linearGradient>
 <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe08a"/><stop offset="1" stop-color="#c9971c"/></linearGradient>
 <linearGradient id="chip" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3b3f45"/><stop offset="1" stop-color="#1c1e22"/></linearGradient>
 <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f8f9fb"/><stop offset="1" stop-color="#eef1f5"/></linearGradient>
 <linearGradient id="wbtitle" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#6d8ac0"/><stop offset="1" stop-color="#4a6aa5"/></linearGradient>
 <filter id="sh" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="2" dy="3" stdDeviation="3" flood-opacity=".28"/></filter>
 <marker id="arO" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#e8590c"/></marker>
 <marker id="arR" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#d6336c"/></marker>
 <marker id="arB" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#1c7ed6"/></marker>
 <marker id="arG" markerWidth="10" markerHeight="10" refX="8" refY="5" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#495057"/></marker>
 <pattern id="via" width="14" height="14" patternUnits="userSpaceOnUse"><circle cx="7" cy="7" r="1.1" fill="#ffffff" opacity=".18"/></pattern>
</defs>''')
rect(0, 0, W, H, 'url(#bg)')

# ---- title -------------------------------------------------------------------------------
text(40, 52, "Alice68's expansion cards", 34, '#1d2433', weight='bold')
text(40, 82, "How the Raspberry Pi 4's USB, WiFi and Ethernet reach AmigaOS: the Alice68 kernel builds two Zorro II cards that Kickstart finds like real ones.", 17, '#495057')

cols = [(30, 'RASPBERRY PI 4 HARDWARE', 'what is really there, driven by the Alice68 kernel'),
        (610, 'VIRTUAL ZORRO II CARDS', 'shared memory the kernel and the drivers both use'),
        (1270, 'AMIGAOS', '68k code, running on CPU0 (Emu68)')]
for x, t, s in cols:
    text(x, 116, t, 15, '#868e96', weight='bold', extra='letter-spacing="2"')
    text(x, 135, s, 13, '#868e96')

# ---- 1. the Pi --------------------------------------------------------------------------------
px, py, pw, ph = 30, 165, 540, 690
rect(px, py, pw, ph, 'url(#pcb)', '#145028', 2, 22, 'filter="url(#sh)"')
rect(px, py, pw, ph, 'url(#via)', rx=22)
for cx, cy in ((px + 26, py + 26), (px + pw - 26, py + 26), (px + 26, py + ph - 26), (px + pw - 26, py + ph - 26)):
    a('<circle cx="%g" cy="%g" r="11" fill="#d4af37"/><circle cx="%g" cy="%g" r="6" fill="#eef1f5"/>' % (cx, cy, cx, cy))
text(px + 60, py + 42, 'Raspberry Pi 4', 20, '#e9f5ec', weight='bold')
text(px + 60, py + 62, 'Pi 400 and CM4 alike (CM4: other WiFi chip)', 12, '#bfe3c9')

# SoC with four cores
sx, sy, sw_, sh_ = px + 120, py + 245, 270, 270
rect(sx - 8, sy - 8, sw_ + 16, sh_ + 16, '#20252b', rx=8, extra='filter="url(#sh)"')
rect(sx, sy, sw_, sh_, 'url(#metal)', '#868e96', 1, 6)
text(sx + sw_ / 2, sy + 22, 'BCM2711  -  4 ARM cores', 13, '#343a40', 'middle', 'bold')
cores = [('CPU0', 'Emu68: the 68k', 'JIT - runs AmigaOS', '#adb5bd', '#212529'),
         ('CPU1', 'Alice68 I/O kernel', 'USB, WiFi, Ethernet', ORANGE, '#ffffff'),
         ('CPU2', 'Display', 'HVS / HDMI', '#adb5bd', '#212529'),
         ('CPU3', 'AGA chipset', 'Alice Lisa Paula', '#adb5bd', '#212529')]
for i, (n, l1, l2, c, tc) in enumerate(cores):
    cx = sx + 12 + (i % 2) * 127
    cy = sy + 34 + (i // 2) * 113
    rect(cx, cy, 119, 103, c, '#495057', 1, 6)
    text(cx + 60, cy + 26, n, 18, tc, 'middle', 'bold')
    text(cx + 60, cy + 52, l1, 12, tc, 'middle', 'bold')
    text(cx + 60, cy + 70, l2, 11.5, tc, 'middle')
    if n == 'CPU1': text(cx + 60, cy + 90, 'own core, never waits', 10.5, tc, 'middle')
    if n == 'CPU0': text(cx + 60, cy + 90, 'translated to ARM', 10.5, tc, 'middle')
c1l, c1r, c1t, c1b = sx + 139, sx + 258, sy + 34, sy + 137   # CPU1's box

# RAM under the SoC
rect(sx, sy + sh_ + 26, 250, 54, 'url(#chip)', rx=4, extra='filter="url(#sh)"')
text(sx + 125, sy + sh_ + 49, 'LPDDR4 RAM - one memory', 13, '#f8f9fa', 'middle', 'bold')
text(sx + 125, sy + sh_ + 67, 'the kernel reads Amiga buffers in place', 11, '#ced4da', 'middle')

# WiFi can
wx, wy = px + 40, py + 100
rect(wx, wy, 150, 92, 'url(#metal)', '#868e96', 1.5, 6, 'filter="url(#sh)"')
text(wx + 75, wy + 30, 'CYW43455', 15, '#343a40', 'middle', 'bold')
text(wx + 75, wy + 50, 'WiFi 2.4 / 5 GHz', 12, '#495057', 'middle')
text(wx + 75, wy + 68, 'on SDIO', 12, '#495057', 'middle')
path('M%g %g q 20 -26 40 0 t 40 0 t 40 0' % (wx + 15, wy - 6), '#d4af37', 3)

# USB-A ports, the VL805 behind them
ex = px + pw - 70
def usb(y, blue):
    rect(ex, y, 84, 62, 'url(#metal)', '#6c757d', 1.5, 4, 'filter="url(#sh)"')
    for k in range(2):
        rect(ex + 12, y + 8 + k * 27, 60, 18, '#1f2328', rx=2)
        rect(ex + 18, y + 12 + k * 27, 48, 8, '#1c7ed6' if blue else '#f1f3f5', rx=1)
usb(py + 92, False); usb(py + 166, True)
text(ex + 42, py + 250, 'USB-A x4', 13, '#ffffff', 'middle', 'bold')
text(ex + 42, py + 266, '2.0 + 3.0', 11, '#d3f9d8', 'middle')
vx, vy = px + 300, py + 135
rect(vx, vy, 120, 54, 'url(#chip)', rx=4, extra='filter="url(#sh)"')
text(vx + 60, vy + 24, 'VL805', 13, '#fff', 'middle', 'bold')
text(vx + 60, vy + 41, 'USB 3 host', 10.5, '#ced4da', 'middle')
line(ex, py + 123, vx + 120, vy + 20, '#d4af37', 3)
line(ex, py + 197, vx + 120, vy + 38, '#d4af37', 3)

# Ethernet: RJ45 and its PHY
ry = py + 470
rect(ex, ry, 84, 76, 'url(#metal)', '#6c757d', 1.5, 4, 'filter="url(#sh)"')
rect(ex + 14, ry + 14, 56, 48, '#1f2328', rx=3)
rect(ex + 26, ry + 52, 32, 10, '#495057')
a('<circle cx="%g" cy="%g" r="4" fill="#51cf66"/><circle cx="%g" cy="%g" r="4" fill="#fcc419"/>' % (ex + 12, ry + 8, ex + 72, ry + 8))
text(ex + 42, ry + 96, 'Gigabit', 13, '#fff', 'middle', 'bold')
text(ex + 42, ry + 112, 'Ethernet', 13, '#fff', 'middle', 'bold')
phx, phy = px + 430, py + 370
rect(phx, phy, 100, 60, 'url(#chip)', rx=4, extra='filter="url(#sh)"')
text(phx + 50, phy + 23, 'BCM54213PE', 11, '#fff', 'middle', 'bold')
text(phx + 50, phy + 40, 'Ethernet PHY', 10.5, '#ced4da', 'middle')
line(ex + 20, ry, phx + 70, phy + 60, '#d4af37', 3)

# SD card
sdy = py + ph - 100
rect(px + 40, sdy, 150, 62, 'url(#metal)', '#868e96', 1.5, 6, 'filter="url(#sh)"')
rect(px + 54, sdy + 9, 100, 44, '#1f2328', rx=4)
text(px + 104, sdy + 36, 'SD card', 13, '#fff', 'middle', 'bold')
lines(px + 205, sdy + 18, ['kernel8.img, config.txt, ALICE68/', '(drivers + installer), WIFI/ (WiFi', 'firmware), your *.hdf hardfiles'], 11.5, 15, fill='#e9f5ec')

# traces to CPU1
for d_, lab, lx, ly in (
    ('M%g %g C %g %g %g %g %g %g' % (wx + 150, wy + 46, wx + 240, wy + 46, c1l + 20, c1t - 80, c1l + 30, c1t), 'SDIO', wx + 175, wy + 38),
    ('M%g %g C %g %g %g %g %g %g' % (vx + 60, vy + 54, vx + 60, vy + 120, c1l + 70, c1t - 60, c1l + 75, c1t), 'PCIe', vx + 72, vy + 80),
    ('M%g %g C %g %g %g %g %g %g' % (phx, phy + 30, phx - 20, phy + 30, c1r + 30, c1t + 70, sx + sw_ + 8, c1t + 60), 'GENET', phx - 12, phy + 52)):
    path(d_, ORANGE, 4, extra='stroke-linecap="round" opacity=".9"')
    text(lx, ly, lab, 11, '#ffd8a8', 'start', 'bold')

# CPU1 -> the Zorro bus (both cards hang on it)
bus_y = 815
path('M%g %g L %g %g L %g %g L %g %g' % (sx + sw_ + 8, c1t + 20, 588, c1t + 20, 588, bus_y + 22, 598, bus_y + 22), ORANGE, 5, extra='marker-end="url(#arO)" stroke-linejoin="round"')
text(sx + sw_ + 16, c1t + 12, 'CPU1 serves both cards', 12, '#ffffff', weight='bold')
badge(588, 640, 2)
# keyboards and mice: CPU1 -> the lane below
path('M%g %g L %g %g L %g %g' % (c1r, c1b - 14, 436, c1b - 14, 436, 878), ORANGE, 3, extra='marker-end="url(#arO)" stroke-dasharray="6 5"')
# ---- 2. the cards ---------------------------------------------------------------------------------
rect(600, bus_y, 640, 44, '#343a40', rx=6, extra='filter="url(#sh)"')
for k in range(3):
    rect(615 + k * 210, bus_y + 8, 200, 12, '#111', rx=2)
text(920, bus_y + 37, 'virtual Zorro II bus  -  Kickstart autoconfigures it at every boot', 12.5, '#f1f3f5', 'middle', 'bold')

def card(x, w, grad, edge, name, ident, size, bands, top=184):
    h = bus_y + 14 - top
    rect(x, top, w, h, grad, edge, 2, 10, 'filter="url(#sh)"')
    rect(x, top, w, h, 'url(#via)', rx=10)
    for k in range(int((w - 40) / 12)):
        rect(x + 20 + k * 12, bus_y + 2, 8, 22, 'url(#gold)', rx=1)
    rect(x + 14, top + 14, w - 28, 66, '#f8f9fa', rx=6)
    text(x + w / 2, top + 38, name, 17, '#1d2433', 'middle', 'bold')
    text(x + w / 2, top + 57, ident, 12, '#495057', 'middle', family='mono')
    text(x + w / 2, top + 73, size, 11.5, '#495057', 'middle')
    text(x + 18, top + 104, 'MEMORY MAP (offset on the card)', 10.5, '#e9ecef', weight='bold', extra='letter-spacing="1"')
    y = top + 114
    for off, title, sub, hh, col in bands:
        rect(x + 14, y, w - 28, hh - 4, col, rx=4)
        text(x + 22, y + 16, off, 11, '#1d2433', family='mono', weight='bold')
        text(x + 86, y + 16, title, 12, '#1d2433', weight='bold')
        if sub: lines(x + 86, y + 31, sub, 10.5, 13, fill='#343a40')
        y += hh
    return top

USB_BANDS = [
    ('0x0000', 'header + 8 port states', ['magic, interrupt flag, what is plugged in'], 48, '#ffe8cc'),
    ('0x0200', '32 request slots, 64 bytes each', ['port, endpoint, length, the 68k', 'buffer address, then the result'], 62, '#ffd8a8'),
    ('0x0C00', 'debug status', None, 26, '#e9ecef'),
    ('0x1000', 'driver log ring  12 KB', ['alice68-usb.device'], 48, '#d0ebff'),
    ('0x4000', 'debug log ring  12 KB', None, 34, '#e9ecef'),
    ('0x7000', 'Alice68-Monitor snapshot', None, 26, '#e9ecef'),
    ('0x8000', 'kernel log ring  32 KB', ['every kernel log line, for', 'Alice68-USBLog and the Monitor'], 76, '#d0ebff'),
]
NET_BANDS = [
    ('0x00000', 'header', ['interrupt flag, firmware hand-over'], 46, '#ffe8cc'),
    ('0x00400', 'unit 0 WiFi: state + commands', ['link, MAC, counters, join/scan'], 46, '#ffd8a8'),
    ('0x00C00', 'unit 1 Ethernet: same', ['speed, duplex, flow control'], 46, '#ffd8a8'),
    ('0x01400', 'WiFi firmware table', None, 26, '#e9ecef'),
    ('0x01800', 'WiFi scan results', None, 26, '#e9ecef'),
    ('0x02000', 'network log ring  8 KB', None, 26, '#d0ebff'),
    ('0x04000', 'receive: 16 slots x 2 KB / unit', ['frames go to the stack in place', '(zero-copy)'], 62, '#c3fae8'),
    ('0x14000', 'send: 12 slots x 2 KB / unit', ['frames the stack sends'], 46, '#c3fae8'),
]
u_x, u_w = 610, 300
n_x, n_w = 925, 300
card(u_x, u_w, 'url(#pcb)', '#145028', 'USB card', 'Zorro II 0x6d73 / 0x6a "USBP"', '64 KB  -  only in kernel8.img', USB_BANDS)
card(n_x, n_w, 'url(#pcb2)', '#0b3a4a', 'Network card', 'Zorro II 0x6d73 / 0x6b', '128 KB  -  in both kernels', NET_BANDS)

# ---- 3. AmigaOS -------------------------------------------------------------------------------------
def win(x, y, w, h, title, body, accent='url(#wbtitle)', size=12.5):
    rect(x, y, w, h, '#f1f3f5', '#495057', 1.5, 3, 'filter="url(#sh)"')
    rect(x, y, w, 24, accent, rx=3)
    rect(x + 6, y + 6, 12, 12, '#f1f3f5', '#2b3a55', 1)
    text(x + 26, y + 17, title, 12.5, '#ffffff', weight='bold')
    lines(x + 12, y + 44, body, size, 16, fill='#1d2433')

ax, aw = 1270, 500
win(ax, 175, aw, 104, 'alice68-usb.device   (DEVS:USBHardware)',
    ['A USB host-controller driver: Neptune sees an 8-port', 'USB 2.0 root hub with every device the kernel offers.', 'Started by  USBStart alice68-usb.device 0'])
win(ax, 292, aw, 74, 'Neptune USB stack   (rondoval)',
    ['Class drivers: USB drives and sticks (DOS volumes),', 'network adapters (SANA-II), and more.'], '#5c7cfa')
win(ax, 395, aw, 114, 'alice68net.device   (DEVS:)',
    ['unit 0 = WiFi        unit 1 = Ethernet', 'netdev interface: the stack and the driver call each', 'other directly; received frames are handed up in place.', 'Settings: Alice68-Network (MUI) or Alice68-NetworkGT.'])
win(ax, 522, aw, 62, 'Riptide 0.38+  or  lwip-amiga   (TCP/IP)',
    ['Browsers, FTP, AmiSSL, ... - everything on the network.'], '#5c7cfa')
win(ax, 600, aw, 78, 'alice68hdf.device   (on Emu68\'s ROM board)',
    ['Hardfiles and SD-card folders. At boot it reads the', 'WIFI folder and hands the firmware to the network card.'], '#868e96')
win(ax, 696, aw, 106, 'The 68k\'s level-2 interrupt   (PORTS)',
    ['Both cards raise it when they have news: a finished', 'request, a new frame, a device plugged in. The drivers\'', 'interrupt code takes it from there.'], '#c2255c')
for y1, y2 in ((292, 279), (522, 509)):
    line(ax + aw / 2, y1, ax + aw / 2, y2 + 4, '#495057', 2, 'marker-end="url(#arG)"')
badge(ax + aw - 18, 708, 4, RED)

# alice68-usb.device <-> the USB card: over the top of the network card
uc = u_x + u_w / 2
path('M%g %g L %g %g L %g %g L %g %g' % (ax - 2, 205, 1250, 205, 1250, 168, uc + 20, 168) + ' L %g %g' % (uc + 20, 176), BLUE, 4, extra='marker-end="url(#arB)" stroke-linejoin="round"')
path('M%g %g L %g %g L %g %g L %g %g L %g %g' % (uc - 20, 180, uc - 20, 156, 1240, 156, 1240, 250, ax - 4, 250), BLUE, 4, extra='marker-end="url(#arB)" stroke-linejoin="round" stroke-dasharray="7 5"')
badge(1080, 168, 1, BLUE)
badge(980, 156, 5, BLUE)
# alice68net.device <-> the network card, side by side
path('M%g %g L %g %g' % (ax - 2, 442, n_x + n_w + 6, 442), BLUE, 4, extra='marker-end="url(#arB)"')
path('M%g %g L %g %g' % (n_x + n_w + 6, 476, ax - 4, 476), BLUE, 4, extra='marker-end="url(#arB)" stroke-dasharray="7 5"')
badge(1248, 442, 1, BLUE)
badge(1248, 476, 5, BLUE)
# the interrupt: from the bus
path('M%g %g C %g %g %g %g %g %g' % (1240, bus_y + 22, 1258, bus_y + 22, 1250, 760, ax - 4, 760), RED, 3.5, extra='marker-end="url(#arR)" stroke-dasharray="10 6"')
badge(1252, 800, 3, RED)

# ---- keyboard / mouse lane --------------------------------------------------------------------------
ky = 880
rect(30, ky, 1740, 62, '#fff4e6', '#ffc078', 1.5, 10)
text(50, ky + 25, 'Keyboards and mice never go through a card:', 14, '#9c4a0b', weight='bold')
text(50, ky + 46, 'the kernel keeps them on CPU1 and feeds them straight into the Amiga\'s own keyboard and mouse ports (virtual CIA) - from power-on, in Kickstart and WHDLoad, with no USB stack in AmigaOS.', 13, '#5f3b10')

# ---- how a request travels ------------------------------------------------------------------------
by = 966
text(30, by + 6, 'HOW ONE REQUEST TRAVELS  (a USB read, or a network frame being sent)', 15, '#868e96', weight='bold', extra='letter-spacing="2"')
steps = [
    (BLUE, 'The Amiga driver writes the request', ['into a free slot on the card: what to do,', 'how much, where the 68k buffer is.']),
    (ORANGE, 'The kernel on CPU1 does it', ['on the real hardware (xHCI, WiFi chip,', 'GENET), reading the Amiga buffer in place.']),
    (RED, 'It raises the Amiga\'s interrupt', ['level 2 (PORTS), as a real Zorro card', 'does when it has finished something.']),
    (RED, 'The driver\'s interrupt code runs', ['it finds the finished slots and', 'replies to Neptune or the TCP/IP stack.']),
    (BLUE, 'The program carries on', ['the 68k never waited for the', 'hardware, and nothing in AmigaOS polls it.']),
]
sw2 = 340
for i, (col, t, sub) in enumerate(steps):
    x = 30 + i * (sw2 + 10)
    rect(x, by + 20, sw2, 98, '#ffffff', '#dee2e6', 1.5, 10, 'filter="url(#sh)"')
    badge(x + 26, by + 50, i + 1, col)
    text(x + 48, by + 55, t, 14, '#1d2433', weight='bold')
    lines(x + 20, by + 82, sub, 12.5, 17, fill='#495057')
    if i < 4:
        line(x + sw2 + 1, by + 69, x + sw2 + 9, by + 69, '#adb5bd', 3)

# ---- good to know ------------------------------------------------------------------------------------
gy = 1112
rect(30, gy, 1740, 220, '#ffffff', '#dee2e6', 1.5, 10, 'filter="url(#sh)"')
text(52, gy + 32, 'GOOD TO KNOW', 15, '#868e96', weight='bold', extra='letter-spacing="2"')
left = [
    ('Alice68\'s own design.', 'No real card has these IDs (0x6d73 is Emu68\'s manufacturer number); only'),
    ('', 'the drivers in ALICE68: know them. They do nothing on a PiStorm or in an emulator.'),
    ('See them:', 'Workbench\'s ShowConfig lists both cards with Emu68\'s ROM board.'),
    ('Logs survive a 68k reset:', 'the cards keep the kernel\'s and the drivers\' log rings, so'),
    ('', 'Alice68-USBLog, Alice68-WiFi LOG and Alice68-Monitor DUMP still read them after a hang.'),
]
right = [
    ('kernel8-nousb.img:', 'no USB card; the network card\'s kernel side has CPU1 to itself.'),
    ('frf_usb=neptune:', 'the USB card stays empty - the kernel leaves the USB-A ports to'),
    ('', 'rondoval\'s xhci.device. Never run xhci.device while the kernel owns them.'),
    ('eth=off:', 'the kernel leaves the Ethernet port alone, for rondoval\'s genet.device.'),
    ('Install:', 'double-click Install in ALICE68: - it puts both drivers and the tools in place.'),
]
for col_x, items in ((52, left), (920, right)):
    for i, (b, t) in enumerate(items):
        yy = gy + 64 + i * 30
        if b:
            text(col_x, yy, b, 13.5, '#1d2433', weight='bold')
            text(col_x + len(b) * 7.0 + 10, yy, t, 13.5, '#343a40')
        else:
            text(col_x + 18, yy, t, 13.5, '#343a40')

a('</svg>')
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'alice68-cards.svg')
open(out, 'w').write('\n'.join(o) + '\n')
print(out)
