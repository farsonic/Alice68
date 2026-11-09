#!/usr/bin/env python3
"""Workbench icons (.info) for the ALICE68 folder, drawn here (original
artwork, Workbench 2/3's four colours: 0 grey, 1 black, 2 white, 3 blue).

    from mkinfo import write_icon, ICONS, read_icon

write_icon() writes a classic DiskObject (planar, two bitplanes, one image,
complemented when selected), which every Workbench from 2.0 reads. read_icon()
reads one back, for the checks in mkfolder.py and the previews.
SPDX-License-Identifier: GPL-2.0-only"""
import struct

WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4
NO_POS = -0x80000000

# ---- drawing --------------------------------------------------------------------
W, H = 56, 26                    # a Workbench screen's pixels are twice as tall as wide

FONT = {                         # 3x5
    'A': ['.#.', '#.#', '###', '#.#', '#.#'], '6': ['.##', '#..', '###', '#.#', '###'],
    '8': ['###', '#.#', '###', '#.#', '###'], 'S': ['.##', '#..', '.#.', '..#', '##.'],
    'D': ['##.', '#.#', '#.#', '#.#', '##.'], 'I': ['###', '.#.', '.#.', '.#.', '###'],
    'O': ['.#.', '#.#', '#.#', '#.#', '.#.'], 'H': ['#.#', '#.#', '###', '#.#', '#.#'],
    'F': ['###', '#..', '##.', '#..', '#..'],
}

class Pic:
    def __init__(s, w=W, h=H): s.w, s.h, s.p = w, h, [[0] * w for _ in range(h)]
    def px(s, x, y, c):
        if 0 <= x < s.w and 0 <= y < s.h: s.p[y][x] = c
    def rect(s, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1): s.px(x, y, c)
    def frame(s, x0, y0, x1, y1, c):
        for x in range(x0, x1 + 1): s.px(x, y0, c); s.px(x, y1, c)
        for y in range(y0, y1 + 1): s.px(x0, y, c); s.px(x1, y, c)
    def text(s, x, y, t, c):
        for ch in t:
            for r, row in enumerate(FONT[ch]):
                for k, b in enumerate(row):
                    if b == '#': s.px(x + k, y + r, c)
            x += 4

def box(p, x0, y0, x1, y1):
    """a parcel: white top, blue front, black edges"""
    t = 4
    for i in range(t):                                   # the top, seen from above
        for x in range(x0 + t - i, x1 - i + 1): p.px(x, y0 + i, 2)
    p.rect(x0, y0 + t, x1 - t, y1, 3)                    # front
    p.rect(x1 - t + 1, y0 + t, x1, y1 - t + 1 + 2, 1)    # side, in shadow
    for i in range(t):
        p.px(x0 + t - i, y0 + i, 1); p.px(x1 - i, y0 + i, 1)
    for x in range(x0 + t, x1 + 1): p.px(x, y0, 1)
    p.frame(x0, y0 + t, x1 - t, y1, 1)
    for x in range(x0, x1 - t + 1): p.px(x, y0 + t, 1)
    p.rect(x0 + 2 + (x1 - x0 - t) // 2 - 1, y0 + t + 1, x0 + 2 + (x1 - x0 - t) // 2, y1 - 1, 2)   # tape

def arrow(p, cx, ytip, down, c=1, fill=2):
    """a fat arrow, tip at (cx, ytip)"""
    d = 1 if down else -1
    rows = []
    for i in range(5): rows.append((ytip - d * i, i * 2 + 1))     # the head
    for i in range(5, 10): rows.append((ytip - d * i, 3))          # the shaft
    for y, half in rows:
        p.rect(cx - half, y, cx + half, y, fill)
        p.px(cx - half - 1, y, c); p.px(cx + half + 1, y, c)
    y_end = ytip - d * 10
    p.rect(cx - 4, y_end, cx + 4, y_end, c)
    p.rect(cx - 10, ytip - d * 5, cx - 4, ytip - d * 5, c); p.rect(cx + 4, ytip - d * 5, cx + 10, ytip - d * 5, c)
    p.px(cx, ytip + d, c)

def icon_install(up=False):
    p = Pic()
    box(p, 10, 12, 44, 25)
    arrow(p, 25, 13 if not up else 2, down=not up)
    return p

def icon_readme():
    p = Pic()
    x0, y0, x1, y1 = 16, 1, 39, 24
    p.rect(x0, y0, x1, y1, 2)
    p.frame(x0, y0, x1, y1, 1)
    for i in range(6):                                   # the folded corner
        p.rect(x1 - 5 + i, y0, x1, y0 + i, 0)
        p.px(x1 - 5 + i, y0 + i, 1)
    p.rect(x1 - 5, y0, x1 - 5, y0 + 5, 1); p.rect(x1 - 5, y0 + 5, x1, y0 + 5, 1)
    for k, y in enumerate(range(y0 + 4, y1 - 1, 3)):
        p.rect(x0 + 3, y, x1 - (8 if k == 0 else 3 + (k * 5) % 7), y, 3 if k == 0 else 1)
    p.px(x1 + 1, y0 + 2, 1); p.rect(x1 + 1, y0 + 2, x1 + 1, y1 + 1, 1); p.rect(x0 + 2, y1 + 1, x1 + 1, y1 + 1, 1)   # shadow
    return p

def icon_disk():
    """an SD card"""
    p = Pic()
    x0, y0, x1, y1 = 14, 1, 41, 24
    p.rect(x0, y0, x1, y1, 3)
    for i in range(7): p.rect(x1 - 6 + i, y0, x1, y0 + i, 0)                   # the cut corner
    for i in range(7): p.px(x1 - 6 + i, y0 + i, 1)
    p.rect(x0, y0, x0, y1, 1); p.rect(x1, y0 + 6, x1, y1, 1); p.rect(x0, y1, x1, y1, 1)
    p.rect(x0, y0, x1 - 6, y0, 1)
    for k in range(5): p.rect(x0 + 3 + k * 4, y0 + 2, x0 + 4 + k * 4, y0 + 6, 2)   # contacts
    p.rect(x0 + 3, y0 + 10, x1 - 3, y1 - 3, 2)                                 # the label
    p.frame(x0 + 3, y0 + 10, x1 - 3, y1 - 3, 1)
    p.text(x0 + 9, y0 + 13, 'A68', 1)
    p.rect(x1 + 1, y0 + 8, x1 + 1, y1 + 1, 1); p.rect(x0 + 2, y1 + 1, x1 + 1, y1 + 1, 1)   # shadow
    return p

def _in_ellipse(x, y, cx, cy, rx, ry):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0

def icon_cdrom():
    """a CD, for the CD images alice68hdf.device mounts"""
    p = Pic()
    cx, cy, rx, ry = 26.0, 11.5, 21.0, 10.4              # a circle on Workbench's tall pixels
    inside = lambda x, y: _in_ellipse(x, y, cx, cy, rx, ry)
    for y in range(H):                                    # shadow, down and to the right
        for x in range(W):
            if not inside(x, y) and inside(x - 2, y - 1): p.px(x, y, 1)
    for y in range(H):
        for x in range(W):
            if not inside(x, y): continue
            edge = not (inside(x - 1, y) and inside(x + 1, y) and inside(x, y - 1) and inside(x, y + 1))
            band = _in_ellipse(x, y, cx, cy, rx - 3, ry - 1.5) and not _in_ellipse(x, y, cx, cy, rx - 5.5, ry - 2.8)
            c = 1 if edge else 3 if band and x > cx + 2 and y < cy - 1 else 2
            if _in_ellipse(x, y, cx, cy, 6.6, 3.2): c = 3                       # the hub
            if _in_ellipse(x, y, cx, cy, 3.8, 1.9): c = 1                       # the hole's rim
            if _in_ellipse(x, y, cx, cy, 2.6, 1.1): c = 0                       # the hole
            p.px(x, y, c)
    p.text(20, 16, 'ISO', 3)
    return p

def icon_sdfolder():
    """a folder with an SD label, for the SD card's folders mounted as volumes"""
    p = Pic()
    x0, y0, x1, y1 = 8, 5, 45, 23
    p.rect(x0, y0 - 3, x0 + 13, y0, 3); p.frame(x0, y0 - 3, x0 + 13, y0, 1)        # the tab
    p.rect(x0 + 3, y0 - 1, x1 - 4, y0 + 3, 2); p.frame(x0 + 3, y0 - 1, x1 - 4, y0 + 3, 1)   # a sheet inside
    p.rect(x0, y0 + 2, x1, y1, 3); p.frame(x0, y0 + 2, x1, y1, 1)                  # the front
    p.rect(x0 + 1, y0 + 3, x1 - 1, y0 + 3, 2)                                      # its top edge, lit
    p.rect(x0 + 13, y0 + 8, x0 + 25, y0 + 15, 2); p.frame(x0 + 13, y0 + 8, x0 + 25, y0 + 15, 1)   # the label
    p.text(x0 + 16, y0 + 10, 'SD', 1)
    p.rect(x1 + 1, y0 + 4, x1 + 1, y1 + 1, 1); p.rect(x0 + 2, y1 + 1, x1 + 1, y1 + 1, 1)   # shadow
    return p

def icon_hardfile():
    """a hard drive marked HDF, to copy onto a hardfile's volume as its Disk.info"""
    p = Pic()
    x0, y0, x1, y1 = 6, 6, 47, 21
    t = 4
    for i in range(t):                                   # the top, seen from above
        p.rect(x0 + t - i, y0 + i, x1 - i, y0 + i, 2)
        p.px(x0 + t - i, y0 + i, 1); p.px(x1 - i, y0 + i, 1)
    p.rect(x0 + t, y0, x1, y0, 1)
    p.rect(x0, y0 + t, x1 - t, y1, 3); p.frame(x0, y0 + t, x1 - t, y1, 1)          # front
    p.rect(x1 - t + 1, y0 + t, x1, y1 - t + 1, 1)                                  # side, in shadow
    for i in range(t): p.px(x1 - t + 1 + i, y1 - i, 1)
    p.rect(x0 + 2, y0 + t + 2, x0 + 18, y0 + t + 10, 2); p.frame(x0 + 2, y0 + t + 2, x0 + 18, y0 + t + 10, 1)   # the label
    p.text(x0 + 5, y0 + t + 5, 'HDF', 1)
    p.rect(x1 - t - 6, y1 - 3, x1 - t - 4, y1 - 2, 2)                              # the activity light
    p.rect(x0 + 21, y0 + t + 5, x1 - t - 3, y0 + t + 5, 1)                         # grille
    p.rect(x0 + 21, y0 + t + 7, x1 - t - 3, y0 + t + 7, 1)
    return p

# ---- the file -------------------------------------------------------------------------
def _image(p):
    rowbytes = (p.w + 15) // 16 * 2
    planes = b''
    for plane in range(2):
        for y in range(p.h):
            row = bytearray(rowbytes)
            for x in range(p.w):
                if p.p[y][x] >> plane & 1: row[x >> 3] |= 0x80 >> (x & 7)
            planes += bytes(row)
    hdr = struct.pack('>hhhhhIBBI', 0, 0, p.w, p.h, 2, 1, 3, 0, 0)
    return hdr + planes

def _str(s):
    b = s.encode('latin1') + b'\0'
    return struct.pack('>I', len(b)) + b

def write_icon(path, kind, pic, tool=None, tooltypes=(), stack=0, pos=None, window=None):
    x, y = pos if pos else (NO_POS, NO_POS)
    gadget = struct.pack('>IhhhhHHHIIIiIHI',
                         0, 0, 0, pic.w, pic.h, 4, 3, 1,      # next, box, GADGIMAGE + complement, RELVERIFY|IMMEDIATE, BOOLGADGET
                         1, 0, 0, 0, 0, 0, 0)                     # GadgetRender (an image follows), no select image; UserData 0: an OS 2 icon
    dobj = struct.pack('>HH', 0xe310, 1) + gadget + struct.pack('>BBIIiiIII',
                         kind, 0, 1 if tool else 0, 1 if tooltypes else 0, x, y,
                         1 if kind in (WBDISK, WBDRAWER) else 0, 0, stack)
    assert len(dobj) == 78
    out = dobj
    if kind in (WBDISK, WBDRAWER):                          # the window it opens: NewWindow, then CurrentX/Y
        wx, wy, ww, wh = window or (60, 30, 360, 110)
        out += struct.pack('>hhhhBBIIIIIIIhhHHH', wx, wy, ww, wh, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 90, 40, 0xffff, 0xffff, 1)
        out += struct.pack('>ii', 0, 0)
    out += _image(pic)
    if tool: out += _str(tool)
    if tooltypes:
        out += struct.pack('>I', (len(tooltypes) + 1) * 4)
        for t in tooltypes: out += _str(t)
    if path: open(path, 'wb').write(out)
    return out

def read_icon(data):
    """parse one back: (kind, Pic, tool, tooltypes, stack); raises on anything malformed"""
    magic, ver = struct.unpack_from('>HH', data, 0)
    if magic != 0xe310 or ver != 1: raise ValueError('not an icon')
    g = struct.unpack_from('>IhhhhHHHIIIiIHI', data, 4)
    render, select = g[8], g[9]
    kind, _, tool, tts, cx, cy, dd, twin, stack = struct.unpack_from('>BBIIiiIII', data, 48)
    o = 78
    if dd: o += 56
    if not render: raise ValueError('no image')
    def img(o):
        le, te, w, h, depth, idata, pick, onoff, nxt = struct.unpack_from('>hhhhhIBBI', data, o)
        o += 20
        p = Pic(w, h)
        rb = (w + 15) // 16 * 2
        for plane in range(depth):
            for y in range(h):
                for x in range(w):
                    if data[o + (plane * h + y) * rb + (x >> 3)] & (0x80 >> (x & 7)): p.p[y][x] |= 1 << plane
        return p, o + depth * h * rb
    pic, o = img(o)
    if select: _, o = img(o)
    def rstr(o):
        n = struct.unpack_from('>I', data, o)[0]
        s = data[o + 4:o + 4 + n]
        if not n or s[-1] != 0: raise ValueError('bad string')
        return s[:-1].decode('latin1'), o + 4 + n
    t = None
    if tool: t, o = rstr(o)
    tt = []
    if tts:
        n = struct.unpack_from('>I', data, o)[0] // 4 - 1; o += 4
        for _ in range(n):
            s, o = rstr(o); tt.append(s)
    if twin: _, o = rstr(o)
    if o != len(data): raise ValueError('%d bytes left over' % (len(data) - o))
    return kind, pic, t, tt, stack

PALETTE = [(0xaa, 0xaa, 0xaa), (0, 0, 0), (0xff, 0xff, 0xff), (0x66, 0x88, 0xbb)]   # Workbench 3.x's

ICONS = {   # name in the folder: (type, picture, default tool, tool types, stack, position)
    'Disk': (WBDISK, icon_disk, None, (), 0, None),
    'Install': (WBPROJECT, icon_install, 'Installer', ('APPNAME=Alice68', 'MINUSER=NOVICE', 'DEFUSER=NOVICE'), 16384, (14, 6)),
    'Uninstall': (WBPROJECT, lambda: icon_install(up=True), 'Installer', ('APPNAME=Alice68', 'MINUSER=NOVICE', 'DEFUSER=NOVICE'), 16384, (94, 6)),
    'ReadMe.txt': (WBPROJECT, icon_readme, 'MultiView', (), 8192, (174, 6)),
}
