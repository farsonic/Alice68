#!/usr/bin/env python3
"""AGA-PISTORM's CRT look on the Pi 4's 3D GPU (V3D 4.2): the GPU programs for
arm/aga_v3d.c, printed as C tables - arm/aga_v3d_code.h.

From RGB2RTG's tools/v3d/mkscale.py (0.78-dev13 to dev30, Astair86, 2026),
whose passes these are: the CRT look in two passes, a curved screen, Frame
blend, Glow and Afterglow (RGB2RTG docs/V3D-SHADERS.md has their history and
every fucking mistake made on the way). Assembled with py-videocore6's assembler
(Idein Inc., MIT, tools/v3d/_videocore6).

What is mine: the source pixels. RGB2RTG's frames are RGB888, three bytes a
pixel, each read as the two words around it and shifted together. The core
writes them as 32-bit words, memory bytes A R G B (aga_hvs.c): one aligned
word a pixel, which the GPU reads little-endian as A | R << 8 | G << 16 |
B << 24 - shifted down by 8, the colour is in the low 24 bits, R G B from the
bottom. Every pass treats the three channels alike, so the order is only the
GPU layer's pixel order (aga_hvs.c gpu_plane: ABGR, memory bytes R G B A). Half
the reads a pixel, the shift amounts gone - about bloody time.

  qpu_lines (pass A, a work-group a source line of the crop): each line
  scaled across to the picture's width into the H buffer, 32 bits a pixel:
  each column's two source pixels x0 and x0 + 1 and the second's weight f
  (0..256) from a running 16.16 position, from the column's centre (x + 0.5)
  x step - 0.5; f the fraction for Smooth, sharpened for Sharp: clamp((frac -
  128) x k / 256 + 128, 0, 256), k = 65536 x 256 / step (256 Smooth, or not
  enlarged); past the last pixel x0 stays the last, f 0. Uniforms shm1, src,
  spitch, hbuf, hpitch, nblk, step, start (lane 0's first position, signed),
  k, swm1; then Frame blend's dsrc, Glow's seven.
  qpu_screen (pass B, a work-group an HDMI row): from rowB's entry for the
  row, L | dl << 10 | wa << 12 | wb << 21, the H lines L and L + dl mixed wa :
  wb (the scanlines' darkness in them already), times the column's mask
  factor. Uniforms ohm1, hbuf, hpitch, out, opitch, nblk, rowB, colt.
  qpu_lines_curve, qpu_screen_curve: the same on a curved screen, the
  scanlines from the beam. qpu_glow (pass G, before A): the glow buffer.
  qpu_afterglow (pass C, between A and B): Afterglow.

RGB2RTG's lessons, each a dev build: accumulators r0-r4 only - a write to r5
is a quad broadcast on V3D 4.2; no more than 8 TMU lookups before ldtmu
(Mesa's MAX_TMU_QUEUE_SIZE: 12 gave fine coloured stripes); every table and
uniform the GPU reads byte-swapped by the big-endian ARM (aga_v3d.c) - do not
ask what happens otherwise.

    python3 tools/v3d/mkcrt.py > arm/aga_v3d_code.h      (Python 3.11 or later)

SPDX-License-Identifier: GPL-2.0-only
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _videocore6.assembler import *  # noqa: E402,F403


# Frame blend (RGB2RTG 0.78-dev27): each source pixel the mean of this frame's
# and the frame before's - a game's 50 Hz tricks (Agony's sky, two gradients
# in turn; Apidya's scroll, a pixel every other frame) as a CRT shows the
# bastards. The frame before is dsrc bytes on from this one (the same place in
# another of the core's pages, aga_hvs.c; 0 when there is none - the frame alone).

def blend_const(asm, c7f):
    mov(r0, -1)
    shr(r0, r0, -7)                 # 0x7F (-7: 25)
    shl(r1, r0, 8)
    bor(r0, r0, r1)                 # 0x7F7F
    shl(r1, r0, 8)
    bor(c7f, r0, r1)                # 0x7F7F7F: each byte's low 7 bits


def blend_reads(asm, dsrc, last):
    """the pixel whose word is at r0: it in this frame and in the frame before
    (r0 kept); the last of the pass's reads switches thread"""
    mov(tmua, r0)
    add(r4, r0, dsrc)
    if last:
        mov(tmua, r4, sig=thrsw)
    else:
        mov(tmua, r4)


def blend_pixel(asm, wa_, wc, c7f, dst):
    """the pixel's word in each frame, its colour in the low 24 bits (the
    alpha shifted out), and their mean into dst: (a & b) + ((a ^ b) >> 1 &
    0x7F7F7F) - each byte rounded down, none carrying into the next"""
    shr(r0, wa_, 8)                 # this frame's
    shr(r3, wc, 8)                  # the frame before's
    band(r2, r0, r3)
    bxor(r4, r0, r3)
    shr(r4, r4, 1)
    band(r4, r4, c7f)
    add(dst, r2, r4)


def blend_loads(asm, ws):
    for w in ws:
        nop(sig=ldtmu(w))


# Glow (RGB2RTG 0.78-dev28): a CRT's light spreading around bright pixels.
# qpu_glow (pass G, before pass A) makes the picture's light small and soft: a
# glow buffer pixel for each 8 source pixels across and 4 lines down. Pass A
# then adds it, its four nearest glow pixels mixed by the place between them,
# with the strength gs (0..256) in the line's two weights, each channel kept
# at 255. Uniforms after pass A's own (and Frame blend's dsrc): gl, glpitch,
# ghm1, gs (the setup's only), gwm1, goff (-3.5 source pixels, 16.16), cov
# (0x01000100). Not for the faint-hearted, this one.

def glow_setup(asm, row_r, gl, glpitch, ghm1, gs, c255, c256, gro0, gro1, wy0, wy1):
    """the line's two rows of the glow buffer and their weights, gs in them"""
    shl(r0, row_r, 14)              # the line in glow rows, 16.16: (line - 1.5) / 4
    mov(r1, 3)
    shl(r1, r1, 13)                 # 1.5 rows x 16384
    sub(r0, r0, r1)
    imax(r0, r0, 0)                 # not above the first
    shr(r1, r0, -16)                # the upper row (-16: 16)
    umin(r1, r1, ghm1)
    add(r2, r1, 1)
    umin(r2, r2, ghm1)              # the lower, at most the last
    umul24(r1, r1, glpitch)
    add(gro0, gl, r1)
    umul24(r2, r2, glpitch)
    add(gro1, gl, r2)
    shr(r0, r0, 8)
    band(r0, r0, c255)              # the lower row's share, 0..255
    umul24(r1, r0, gs)
    shr(wy1, r1, 8)
    sub(r0, c256, r0)
    umul24(r1, r0, gs)
    shr(wy0, r1, 8)                 # the upper's, both with the strength


def glow_reads(asm, gu, gwm1, fxg, gro0, gro1, c255):
    """the column's four glow pixels (gu: its place, 3.5 source pixels back,
    16.16) - the last read switches thread; the right ones' share into fxg"""
    asr(r1, gu, 3)                  # in glow pixels
    imax(r1, r1, 0)
    shr(r2, r1, 8)
    band(fxg, r2, c255)             # the right ones' share, 0..255
    shr(r1, r1, -16)                # the left column (-16: 16)
    umin(r1, r1, gwm1)
    add(r2, r1, 1)
    umin(r2, r2, gwm1)              # the right, at most the last
    shl(r1, r1, 2)
    shl(r2, r2, 2)
    add(r3, gro0, r1)
    mov(tmua, r3)
    add(r3, gro0, r2)
    mov(tmua, r3)
    add(r3, gro1, r1)
    mov(tmua, r3)
    add(r3, gro1, r2)
    mov(tmua, r3, sig=thrsw)


def glow_mix(asm, g, fxg, wy0, wy1, mbr, mg, c256, gbr, gg):
    """the four mixed - across by fxg, down by wy0 : wy1 - two channels
    together (16 bits a field: no carry) into gbr, the middle one into gg"""
    sub(r0, c256, fxg)              # the left ones' share
    for mask, dst in ((mbr, gbr), (mg, gg)):
        band(r1, g[0], mask)
        umul24(r1, r1, r0)
        band(r2, g[1], mask)
        umul24(r2, r2, fxg)
        add(r1, r1, r2)
        shr(r1, r1, 8)
        band(r1, r1, mask)          # the upper row's
        band(r2, g[2], mask)
        umul24(r2, r2, r0)
        band(r3, g[3], mask)
        umul24(r3, r3, fxg)
        add(r2, r2, r3)
        shr(r2, r2, 8)
        band(r2, r2, mask)          # the lower row's
        umul24(r1, r1, wy0)
        umul24(r2, r2, wy1)
        add(r1, r1, r2)
        shr(r1, r1, 8)
        band(dst, r1, mask)


def glow_add(asm, gbr, gg, cov, c255, mbr, mg):
    """the glow added to the pixel's outer channels (r1) and middle one (r4),
    each at most 255"""
    add(r1, r1, gbr)                # up to 510 a field
    band(r2, r1, cov)               # the fields past 255
    shr(r2, r2, 8)
    umul24(r2, r2, c255)            # 0xFF in each of them
    bor(r1, r1, r2)
    band(r1, r1, mbr)
    add(r4, r4, gg)
    shr(r2, r4, -16)                # 1 past 255 (-16: 16)
    umul24(r2, r2, mg)
    bor(r4, r4, r2)
    band(r4, r4, mg)


@qpu
def qpu_glow(asm):
    # Glow's pass G: each glow buffer pixel the mean of 16 source pixels of its
    # 8 x 4 - every other column (a lores pixel each), 4 lines - squared per
    # channel and / 256, so that the bright ones glow and the dark ones hardly
    # do, which is the whole bloody point. A work-group a row of it; uniforms
    # ghm1, src, spitch, gl, glpitch, nblk (blocks of 16 of its pixels), swm1,
    # shm1. A source pixel one word.
    row_j = rf1
    ghm1, src, spitch, gl, glpitch, nblk, swm1, shm1 = rf2, rf3, rf4, rf5, rf6, rf7, rf8, rf9
    lane, line0, xb, oaddr, blk, ln, cnt, rowa = rf10, rf11, rf12, rf13, rf14, rf15, rf16, rf17
    sb, sg, sr, c255, c64, c128 = rf18, rf19, rf20, rf21, rf22, rf23
    ws = (rf24, rf25, rf26, rf27)

    shl(r0, rf0, -16)
    shr(row_j, r0, -16)             # this batch's row of the glow buffer: its work-group's X
    for r in (ghm1, src, spitch, gl, glpitch, nblk, swm1, shm1):
        nop(sig=ldunifrf(r))
    umin(row_j, row_j, ghm1)        # never past its last row
    eidx(lane)
    shl(line0, row_j, 2)            # its first source line: 4 a row
    umul24(r0, row_j, glpitch)
    add(r0, gl, r0)
    shl(r1, lane, 2)
    add(oaddr, r0, r1)              # this lane's first pixel in it
    shl(r0, lane, 3)
    add(xb, r0, 1)                  # ... and its first source column (a pair's second)
    mov(blk, nblk)
    mov(r0, 1)
    shl(c64, r0, 6)                 # 64: 16 lanes x 4 bytes
    shl(c128, r0, 7)                # 128: 16 lanes x 8 source pixels
    shl(r1, r0, 8)
    sub(c255, r1, 1)                # 255

    with loop as block:
        mov(sb, 0)
        mov(sg, 0)
        mov(sr, 0)
        mov(ln, line0)
        mov(cnt, 4)
        with loop as lines:
            umin(r0, ln, shm1)      # never past the crop's last line
            umul24(r0, r0, spitch)
            add(rowa, src, r0)      # the line's first byte
            for i in range(4):
                add(r0, xb, 2 * i)
                umin(r0, r0, swm1)  # never past the last pixel
                shl(r0, r0, 2)      # x 4 bytes
                add(r0, rowa, r0)
                if i < 3:
                    mov(tmua, r0)   # its word
                else:
                    mov(tmua, r0, sig=thrsw)
            add(ln, ln, 1)          # (delay slot) the next line
            nop()                   # (delay slot)
            for w in ws:
                nop(sig=ldtmu(w))
            for i in range(4):
                shr(r0, ws[i], 8)   # the pixel: its colour in the low 24 bits
                band(r1, r0, c255)
                umul24(r1, r1, r1)
                add(sb, sb, r1)     # the low channel squared, summed
                shr(r1, r0, 8)
                band(r1, r1, c255)
                umul24(r1, r1, r1)
                add(sg, sg, r1)
                shr(r1, r0, -16)    # (-16: 16)
                band(r1, r1, c255)
                umul24(r1, r1, r1)
                add(sr, sr, r1)
            sub(cnt, cnt, 1, cond="pushz")
            lines.b(cond="anyna")
            nop()                   # (delay slot)
            nop()                   # (delay slot)
            nop()                   # (delay slot)
        shr(r0, sb, 12)             # the 16 squares' mean / 256: 0..254
        shr(r1, sg, 12)
        shl(r1, r1, 8)
        bor(r0, r0, r1)
        shr(r1, sr, 12)
        shl(r1, r1, -16)            # (-16: 16)
        bor(tmud, r0, r1)
        mov(tmua, oaddr)            # into the glow buffer
        sub(blk, blk, 1, cond="pushz")
        block.b(cond="anyna")
        add(oaddr, oaddr, c64)      # (delay slot) the next block's pixel
        add(xb, xb, c128)           # (delay slot) ... and its first column
        nop()                       # (delay slot)

    tmuwt()                         # the last writes done
    nop(sig=thrsw)                  # the end
    nop(sig=thrsw)
    nop()
    nop()
    nop(sig=thrsw)
    nop()
    nop()
    nop()


def pixel_pair(asm, row_src, x0, blend, glow, dsrc, gu, gwm1, fxg, gro0, gro1, c255,
               w0, w2, w4, w6, c7f, p0, g, wy0, wy1, mbr, mg, c256, gbr, gg, delay):
    """the column's two source pixels x0 and x0 + 1 read - with the frame
    before's, and the glow's four - into r0 and r3, the colour in the low 24
    bits; delay: the thread switch's two delay slots"""
    shl(r0, x0, 2)                  # x0 x 4 bytes
    add(r0, row_src, r0)            # the first pixel's word
    if blend:
        blend_reads(asm, dsrc, False)
        add(r0, r0, 4)              # the second's
        blend_reads(asm, dsrc, True)
        delay()
        blend_loads(asm, (w0, w4, w2, w6))      # in the reads' order
        if glow:
            # (RGB2RTG 0.78-dev29) the glow's four reads once those are in:
            # no more than 8 lookups at a time
            glow_reads(asm, gu, gwm1, fxg, gro0, gro1, c255)
            nop()                   # (delay slot)
            nop()                   # (delay slot)
        blend_pixel(asm, w0, w4, c7f, p0)       # the first pixel, both frames'
        if glow:
            blend_loads(asm, g)
            glow_mix(asm, g, fxg, wy0, wy1, mbr, mg, c256, gbr, gg)
        blend_pixel(asm, w2, w6, c7f, r3)       # the second
        mov(r0, p0)
    else:
        mov(tmua, r0)               # its word
        add(r2, r0, 4)              # the second's
        if glow:
            mov(tmua, r2)
            glow_reads(asm, gu, gwm1, fxg, gro0, gro1, c255)
        else:
            mov(tmua, r2, sig=thrsw)
        delay()
        nop(sig=ldtmu(w0))
        nop(sig=ldtmu(w2))
        if glow:
            blend_loads(asm, g)
            glow_mix(asm, g, fxg, wy0, wy1, mbr, mg, c256, gbr, gg)
        shr(r0, w0, 8)              # the first pixel: its colour in the low 24 bits
        shr(r3, w2, 8)              # the second


def mix_pair(asm, wa, fx, mbr, mg, t5, glow, gbr, gg, cov, c255):
    """r0 and r3 mixed wa : fx - the outer channels a multiply together (16
    bits a field), the middle one alone - into r1 | r4"""
    band(r1, r0, mbr)
    band(r2, r3, mbr).umul24(r1, r1, wa)
    band(r4, r0, mg).umul24(r2, r2, fx)
    band(t5, r3, mg).umul24(r4, r4, wa)     # (not r5: a write to it is a quad broadcast)
    add(r1, r1, r2).umul24(t5, t5, fx)
    add(r4, r4, t5)
    shr(r1, r1, 8)
    shr(r4, r4, 8)
    band(r1, r1, mbr)
    band(r4, r4, mg)
    if glow:
        glow_add(asm, gbr, gg, cov, c255, mbr, mg)


def masks(asm, ctop, mbr, mg):
    mov(r0, -1)
    shl(r1, r0, 12)
    shl(ctop, r1, 12)               # 0xFF000000
    shr(r0, r0, -8)                 # 0xFF (-8: 24)
    shl(r1, r0, -16)                # 0xFF0000
    bor(mbr, r0, r1)                # 0x00FF00FF: the outer channels
    shl(mg, r0, 8)                  # 0x0000FF00: the middle one


def the_end(asm):
    tmuwt()                         # the last writes done
    nop(sig=thrsw)                  # the end
    nop(sig=thrsw)
    nop()
    nop()
    nop(sig=thrsw)
    nop()
    nop()
    nop()


@qpu
def qpu_lines(asm, blend=False, glow=False):
    row_r = rf1
    shm1, src, spitch, hbuf, hpitch, nblk, step, start, k, swm1 = rf2, rf3, rf4, rf5, rf6, rf7, rf8, rf9, rf10, rf11
    lane, row_src, oaddr, blk, c64, mbr, mg, ctop = rf12, rf13, rf14, rf15, rf16, rf17, rf18, rf19
    c256, sx, step16, fx, wa = rf20, rf21, rf22, rf23, rf24
    w0, w2, c255, x0, c128, t5 = rf25, rf26, rf27, rf28, rf29, rf30
    dsrc, w4, w6, c7f, p0 = rf31, rf32, rf33, rf34, rf35                    # Frame blend's
    gwm1, goff, cov, gro0, gro1, wy0, wy1, gu = rf36, rf37, rf38, rf39, rf40, rf41, rf42, rf43     # Glow's
    fxg, g0, g1, g2, g3, gbr, gg = rf44, rf45, rf46, rf47, rf48, rf49, rf50

    shl(r0, rf0, -16)               # (-16: a shift takes 5 bits, 16)
    shr(row_r, r0, -16)             # this batch's source line: its work-group's X
    for r in ((shm1, src, spitch, hbuf, hpitch, nblk, step, start, k, swm1) + ((dsrc,) if blend else ()) +
              ((g0, g1, g2, g3, gwm1, goff, cov) if glow else ())):     # (gl, glpitch, ghm1, gs: the setup's)
        nop(sig=ldunifrf(r))
    umin(row_r, row_r, shm1)        # never past the crop's last line
    eidx(lane)
    umul24(r0, row_r, spitch)
    add(row_src, src, r0)           # the line's first byte
    umul24(r0, row_r, hpitch)
    add(r0, hbuf, r0)
    shl(r1, lane, 2)
    add(oaddr, r0, r1)              # this lane's first pixel in H
    umul24(r0, lane, step)
    add(sx, start, r0)              # its first position: (lane + 0.5) x step - 0.5, 16.16, signed
    shl(step16, step, 4)            # 16 columns' step
    mov(blk, nblk)
    mov(r0, 1)
    shl(c64, r0, 6)                 # 64: 16 lanes x 4 bytes
    shl(c256, r0, 8)                # 256
    sub(c255, c256, 1)              # 255
    shl(c128, r0, 7)                # 128
    masks(asm, ctop, mbr, mg)
    if blend:
        blend_const(asm, c7f)
    if glow:
        glow_setup(asm, row_r, g0, g1, g2, g3, c255, c256, gro0, gro1, wy0, wy1)

    def delay():
        sub(wa, c256, fx)           # (delay slot) the first pixel's weight
        add(sx, sx, step16)         # (delay slot)

    with loop as block:
        imax(r0, sx, 0)             # not left of the first pixel
        if glow:
            add(gu, r0, goff)       # (Glow: its place for the glow buffer)
        shr(r1, r0, -16)            # x0 (-16: 16)
        shr(r2, r0, 8)
        band(r2, r2, c255)          # the fraction, 0..255
        sub(r2, r2, c128)
        smul24(r2, r2, k)           # (frac - 128) x k
        asr(r2, r2, 8)
        add(r2, r2, c128)
        imax(r2, r2, 0)
        imin(fx, r2, c256)          # the second pixel's weight, 0..256
        sub(r3, r1, swm1, cond="pushn")         # left of the last pixel?
        mov(x0, swm1)
        mov(x0, r1, cond="ifa")     # x0, at most the last
        mov(fx, 0, cond="ifna")     # at or past the last: it alone
        pixel_pair(asm, row_src, x0, blend, glow, dsrc, gu, gwm1, fxg, gro0, gro1, c255,
                   w0, w2, w4, w6, c7f, p0, (g0, g1, g2, g3), wy0, wy1, mbr, mg, c256, gbr, gg, delay)
        mix_pair(asm, wa, fx, mbr, mg, t5, glow, gbr, gg, cov, c255)
        bor(r0, r1, r4)
        bor(tmud, r0, ctop)         # alpha opaque
        mov(tmua, oaddr)            # into H
        sub(blk, blk, 1, cond="pushz")
        block.b(cond="anyna")
        add(oaddr, oaddr, c64)      # (delay slot) the next block's pixel
        nop()                       # (delay slot)
        nop()                       # (delay slot)

    the_end(asm)


@qpu
def qpu_afterglow(asm):
    # RGB2RTG 0.78-dev30, Afterglow: pass C, after pass A and before B - each
    # pixel of H at least the frame before's (its own H, a second buffer) faded
    # by decay (0..256), per channel: max(this, before x decay) - a phosphor
    # glowing on where the picture went dark, and fading. Two reads a pixel,
    # the result written over this frame's. A work-group a line of H; uniforms
    # shm1, hcur, hprev, hpitch, nblk, decay. Phosphor does not die instantly,
    # and this is the shit that makes it look right.
    row_r = rf1
    shm1, hcur, hprev, hpitch, nblk, decay = rf2, rf3, rf4, rf5, rf6, rf7
    lane, oaddr, paddr, blk, c64, mbr, mg, ctop = rf8, rf9, rf10, rf11, rf12, rf13, rf14, rf15
    cb, cr, v0, v1 = rf16, rf17, rf18, rf19

    shl(r0, rf0, -16)
    shr(row_r, r0, -16)             # this batch's line: its work-group's X
    for r in (shm1, hcur, hprev, hpitch, nblk, decay):
        nop(sig=ldunifrf(r))
    umin(row_r, row_r, shm1)        # never past the crop's last line
    eidx(lane)
    umul24(r0, row_r, hpitch)
    shl(r1, lane, 2)
    add(r0, r0, r1)
    add(oaddr, hcur, r0)            # this lane's first pixel, this frame's
    add(paddr, hprev, r0)           # ... and the frame before's
    mov(blk, nblk)
    mov(r0, 1)
    shl(c64, r0, 6)                 # 64: 16 lanes x 4 bytes
    mov(r0, -1)
    shl(r1, r0, 12)
    shl(ctop, r1, 12)               # 0xFF000000
    shr(cb, r0, -8)                 # 0xFF: the low channel (-8: 24)
    shl(cr, cb, -16)                # 0xFF0000: the high one
    bor(mbr, cb, cr)                # 0x00FF00FF
    shl(mg, cb, 8)                  # 0x0000FF00: the middle one

    with loop as block:
        mov(tmua, oaddr)            # this frame's pixel
        mov(tmua, paddr, sig=thrsw) # ... and the frame before's
        add(paddr, paddr, c64)      # (delay slot)
        nop()                       # (delay slot)
        nop(sig=ldtmu(v0))
        nop(sig=ldtmu(v1))
        band(r1, v1, mbr)
        umul24(r1, r1, decay)
        shr(r1, r1, 8)
        band(r1, r1, mbr)           # the frame before's outer channels, faded
        band(r2, v1, mg)
        umul24(r2, r2, decay)
        shr(r2, r2, 8)
        band(r2, r2, mg)            # ... and the middle one
        band(r3, v0, cb)
        band(r4, r1, cb)
        imax(r0, r3, r4)            # the low channel, the brighter
        band(r3, v0, cr)
        band(r4, r1, cr)
        imax(r3, r3, r4)            # the high one
        bor(r0, r0, r3)
        band(r3, v0, mg)
        imax(r3, r3, r2)            # the middle one
        bor(r0, r0, r3)
        bor(tmud, r0, ctop)         # alpha opaque
        mov(tmua, oaddr)            # over this frame's
        sub(blk, blk, 1, cond="pushz")
        block.b(cond="anyna")
        add(oaddr, oaddr, c64)      # (delay slot) the next block's pixel
        nop()                       # (delay slot)
        nop()                       # (delay slot)

    the_end(asm)


@qpu
def qpu_screen(asm):
    row_y = rf1
    ohm1, hbuf, hpitch, out, opitch, nblk, rowb, colt = rf2, rf3, rf4, rf5, rf6, rf7, rf8, rf9
    lane, pa, pb, oaddr, maddr, blk, c64, mbr = rf10, rf11, rf12, rf13, rf14, rf15, rf16, rf17
    mg, ctop, e, wa, wb, va, vb, m = rf18, rf19, rf20, rf21, rf22, rf23, rf24, rf25
    c1023, c511, q = rf26, rf27, rf28

    shl(r0, rf0, -16)
    shr(row_y, r0, -16)             # this batch's HDMI row: its work-group's X
    for r in (ohm1, hbuf, hpitch, out, opitch, nblk, rowb, colt):
        nop(sig=ldunifrf(r))
    umin(row_y, row_y, ohm1)        # never past the picture's last row
    eidx(lane)
    shl(r0, row_y, 2)
    add(r0, rowb, r0)
    mov(tmua, r0, sig=thrsw)        # the row's entry
    nop()                           # (delay slot: accumulators do not survive the switch)
    nop()                           # (delay slot)
    nop(sig=ldtmu(e))
    mov(r0, 1)
    shl(r1, r0, 10)
    sub(c1023, r1, 1)               # 1023
    shl(r1, r0, 9)
    sub(c511, r1, 1)                # 511
    shl(c64, r0, 6)                 # 64: 16 lanes x 4 bytes
    band(r0, e, c1023)              # L
    umul24(r0, r0, hpitch)
    add(r0, hbuf, r0)
    shl(r1, lane, 2)
    add(pa, r0, r1)                 # line L, this lane's first pixel
    shr(r0, e, 10)
    band(r0, r0, 1)                 # dl
    umul24(r0, r0, hpitch)
    add(pb, pa, r0)                 # line L + dl
    shr(r0, e, 12)
    band(wa, r0, c511)              # the first line's weight
    shr(wb, e, -11)                 # the second's (-11: 21)
    umul24(r0, row_y, opitch)
    add(r0, out, r0)
    add(oaddr, r0, r1)              # this lane's first pixel out
    add(maddr, colt, r1)            # ... and its column's factor
    mov(blk, nblk)
    masks(asm, ctop, mbr, mg)

    with loop as block:
        mov(tmua, pa)               # line L's pixel,
        mov(tmua, pb)               # line L + dl's,
        mov(tmua, maddr, sig=thrsw) # and the column's mask factor
        add(pa, pa, c64)            # (delay slot)
        add(pb, pb, c64)            # (delay slot)
        nop(sig=ldtmu(va))
        nop(sig=ldtmu(vb))
        nop(sig=ldtmu(m))
        add(maddr, maddr, c64)
        umul24(r0, wa, m)           # the weights times the mask's factor
        umul24(r1, wb, m)
        shr(r0, r0, 8)
        shr(r1, r1, 8)
        band(r2, va, mbr)           # the outer channels
        band(r3, vb, mbr).umul24(r2, r2, r0)
        band(r4, va, mg).umul24(r3, r3, r1)
        band(q, vb, mg).umul24(r4, r4, r0)     # (not r5: a write to it is a quad broadcast)
        add(r2, r2, r3).umul24(q, q, r1)
        add(r4, r4, q)
        shr(r2, r2, 8)
        shr(r4, r4, 8)
        band(r2, r2, mbr)
        band(r4, r4, mg)
        bor(r0, r2, r4)
        bor(tmud, r0, ctop)         # alpha opaque
        mov(tmua, oaddr)            # the pixel written
        sub(blk, blk, 1, cond="pushz")
        block.b(cond="anyna")
        add(oaddr, oaddr, c64)      # (delay slot) the next block's pixel
        nop()                       # (delay slot)
        nop()                       # (delay slot)

    the_end(asm)


@qpu
def qpu_lines_curve(asm, blend=False, glow=False):
    # RGB2RTG 0.78-dev21: pass A on a curved screen - qpu_lines with each
    # column's place on the flat picture from a barrel: x' = x (1 + kx (x^2 +
    # y^2)) bsx, x and y from the picture's middle (-1/2..1/2), y the line's own
    # (the vertical warp is pass B's). A column off the flat picture, |x'| >
    # 1/2, black. Floats, the constants the kernel's: inv_ow 1/ow, sw16 sw x
    # 65536, off16 (sw / 2 - 1/2) x 65536, inv_sh 1/sh, kx, bsx 1 / (1 + kx /
    # 4) - the middle of the left and right edges stays on them. That is the
    # whole trick, and it is all floating point.
    row_r = rf1
    shm1, src, spitch, hbuf, hpitch, nblk, k, swm1 = rf2, rf3, rf4, rf5, rf6, rf7, rf8, rf9
    inv_ow, sw16, off16, inv_sh, kx, bsx = rf10, rf11, rf12, rf13, rf14, rf15
    lane, row_src, oaddr, blk, c64, mbr, mg, ctop = rf16, rf17, rf18, rf19, rf20, rf21, rf22, rf23
    c256, c255, c128, xf, xstep, yl2, inm = rf24, rf25, rf26, rf27, rf28, rf29, rf30
    wa, fx, x0, w0, w2, t5 = rf31, rf32, rf33, rf34, rf35, rf36
    dsrc, w4, w6, c7f, p0 = rf37, rf38, rf39, rf40, rf41                    # Frame blend's
    gwm1, goff, cov, gro0, gro1, wy0, wy1, gu = rf42, rf43, rf44, rf45, rf46, rf47, rf48, rf49     # Glow's
    fxg, g0, g1, g2, g3, gbr, gg = rf50, rf51, rf52, rf53, rf54, rf55, rf56

    shl(r0, rf0, -16)
    shr(row_r, r0, -16)             # this batch's source line: its work-group's X
    for r in ((shm1, src, spitch, hbuf, hpitch, nblk, k, swm1, inv_ow, sw16, off16, inv_sh, kx, bsx) +
              ((dsrc,) if blend else ()) + ((g0, g1, g2, g3, gwm1, goff, cov) if glow else ())):
        nop(sig=ldunifrf(r))
    umin(row_r, row_r, shm1)        # never past the crop's last line
    eidx(lane)
    umul24(r0, row_r, spitch)
    add(row_src, src, r0)           # the line's first byte
    umul24(r0, row_r, hpitch)
    add(r0, hbuf, r0)
    shl(r1, lane, 2)
    add(oaddr, r0, r1)              # this lane's first pixel in H
    itof(r0, row_r)
    fadd(r0, r0, 0.5)
    fmul(r0, r0, inv_sh)
    fsub(r0, r0, 0.5)               # the line's height on the picture, -1/2..1/2
    fmul(yl2, r0, r0)
    itof(r0, lane)
    fadd(r0, r0, 0.5)
    fmul(r0, r0, inv_ow)
    fsub(xf, r0, 0.5)               # this lane's first column across, the same
    fmul(xstep, inv_ow, 16.0)       # 16 columns'
    mov(blk, nblk)
    mov(r0, 1)
    shl(c64, r0, 6)                 # 64: 16 lanes x 4 bytes
    shl(c256, r0, 8)                # 256
    sub(c255, c256, 1)              # 255
    shl(c128, r0, 7)                # 128
    masks(asm, ctop, mbr, mg)
    if blend:
        blend_const(asm, c7f)
    if glow:
        glow_setup(asm, row_r, g0, g1, g2, g3, c255, c256, gro0, gro1, wy0, wy1)

    def delay():
        sub(wa, c256, fx)           # (delay slot) the first pixel's weight
        fadd(xf, xf, xstep)         # (delay slot)

    with loop as block:
        fmul(r0, xf, xf)
        fadd(r0, r0, yl2)           # r^2
        fmul(r0, r0, kx)
        fadd(r0, r0, 1.0)
        fmul(r0, r0, xf)
        fmul(r1, r0, bsx)           # the column's place on the flat picture
        fmul(r2, r1, r1)
        fsub(r2, r2, 0.25, cond="pushn")    # on it: x'^2 < 1/4
        mov(inm, 0)
        mov(inm, -1, cond="ifa")    # its pixel kept, or black
        fmul(r0, r1, sw16)
        fadd(r0, r0, off16)         # its source position, 16.16
        ftoiz(r0, r0)
        imax(r0, r0, 0)             # not left of the first pixel
        if glow:
            add(gu, r0, goff)       # (Glow: its place for the glow buffer)
        shr(r1, r0, -16)            # x0 (-16: 16)
        shr(r2, r0, 8)
        band(r2, r2, c255)          # the fraction, 0..255
        sub(r2, r2, c128)
        smul24(r2, r2, k)           # (frac - 128) x k
        asr(r2, r2, 8)
        add(r2, r2, c128)
        imax(r2, r2, 0)
        imin(fx, r2, c256)          # the second pixel's weight, 0..256
        sub(r3, r1, swm1, cond="pushn")     # left of the last pixel?
        mov(x0, swm1)
        mov(x0, r1, cond="ifa")     # x0, at most the last
        mov(fx, 0, cond="ifna")     # at or past the last: it alone
        pixel_pair(asm, row_src, x0, blend, glow, dsrc, gu, gwm1, fxg, gro0, gro1, c255,
                   w0, w2, w4, w6, c7f, p0, (g0, g1, g2, g3), wy0, wy1, mbr, mg, c256, gbr, gg, delay)
        mix_pair(asm, wa, fx, mbr, mg, t5, glow, gbr, gg, cov, c255)
        bor(r0, r1, r4)
        band(r0, r0, inm)           # off the curved picture: black
        bor(tmud, r0, ctop)         # alpha opaque
        mov(tmua, oaddr)            # into H
        sub(blk, blk, 1, cond="pushz")
        block.b(cond="anyna")
        add(oaddr, oaddr, c64)      # (delay slot) the next block's pixel
        nop()                       # (delay slot)
        nop()                       # (delay slot)

    the_end(asm)


@qpu
def qpu_screen_curve(asm):
    # RGB2RTG 0.78-dev21/23/25: pass B on a curved screen - each HDMI pixel's
    # source line from the barrel down, y' = y (1 + ky (x^2 + y^2)) bsy (cya = y
    # bsy and cyb = cya ky for the row, worked out once), its two lines of H at
    # s = y' sh + sh / 2 - 1/2 (whole at a line's middle), the second's weight
    # the fraction f sharpened by kv (Sharp: the rows a line; Smooth 1). The
    # scanlines from the beam: a pixel between two lines darker by (f (1 -
    # f))^2 x sc16 (the strength times 16), so that they curve with the
    # picture. Then the column's mask factor (flat). A pixel off the flat
    # picture black. Ugly as hell, and it works.
    row_y = rf1
    ohm1, hbuf, hpitch, out, opitch, nblk, colt, shm1 = rf2, rf3, rf4, rf5, rf6, rf7, rf8, rf9
    inv_ow, inv_oh, ky, bsy, sh_f, offl, kv, sc16 = rf10, rf11, rf12, rf13, rf14, rf15, rf16, rf17
    lane, caddr, oaddr, maddr, blk, c64, mbr, mg = rf18, rf19, rf20, rf21, rf22, rf23, rf24, rf25
    ctop, xf, xstep, y02, cya, cyb, inm, pa = rf26, rf27, rf28, rf29, rf30, rf31, rf32, rf33
    pb, wf, va, vb, m, q, dk = rf34, rf35, rf36, rf37, rf38, rf39, rf40

    shl(r0, rf0, -16)
    shr(row_y, r0, -16)             # this batch's HDMI row: its work-group's X
    for r in (ohm1, hbuf, hpitch, out, opitch, nblk, colt, shm1, inv_ow, inv_oh, ky, bsy, sh_f, offl, kv, sc16):
        nop(sig=ldunifrf(r))
    umin(row_y, row_y, ohm1)        # never past the picture's last row
    eidx(lane)
    shl(r1, lane, 2)
    add(caddr, hbuf, r1)            # this lane's column of H
    add(maddr, colt, r1)            # ... and its column's mask factor
    umul24(r0, row_y, opitch)
    add(r0, out, r0)
    add(oaddr, r0, r1)              # this lane's first pixel out
    itof(r0, row_y)
    fadd(r0, r0, 0.5)
    fmul(r0, r0, inv_oh)
    fsub(r0, r0, 0.5)               # the row's height, -1/2..1/2
    fmul(y02, r0, r0)
    fmul(cya, r0, bsy)
    fmul(cyb, cya, ky)
    itof(r0, lane)
    fadd(r0, r0, 0.5)
    fmul(r0, r0, inv_ow)
    fsub(xf, r0, 0.5)               # this lane's first column across
    fmul(xstep, inv_ow, 16.0)       # 16 columns'
    mov(blk, nblk)
    mov(r0, 1)
    shl(c64, r0, 6)                 # 64: 16 lanes x 4 bytes
    masks(asm, ctop, mbr, mg)

    with loop as block:
        fmul(r0, xf, xf)
        fadd(r0, r0, y02)           # r^2
        fmul(r0, r0, cyb)
        fadd(r1, r0, cya)           # the pixel's height on the flat picture
        fmul(r2, r1, r1)
        fsub(r2, r2, 0.25, cond="pushn").fmul(r0, r1, sh_f)    # on it: y'^2 < 1/4
        mov(inm, 0)
        mov(inm, -1, cond="ifa")
        fadd(r0, r0, offl)          # s, its source line's place
        ffloor(r1, r0)
        fsub(r2, r0, r1)            # f, from line L's middle to L + 1's
        ftoiz(r1, r1)               # L: -1 above line 0's middle
        add(r3, r1, 1)              # L + 1 from L itself (RGB2RTG dev25)
        imax(r1, r1, 0)
        imax(r3, r3, 0)
        umin(r1, r1, shm1)          # both within the crop: at the top line 0 alone,
        umin(r3, r3, shm1)          # ... at the bottom the last alone
        umul24(r1, r1, hpitch)
        add(pa, caddr, r1)          # line L's pixel
        umul24(r3, r3, hpitch)
        add(pb, caddr, r3)          # line L + 1's
        fsub(r4, r2, 0.5).fmul(r3, r2, r2)      # (the sharpening; f^2)
        fsub(r3, r2, r3).fmul(r4, r4, kv)       # f (1 - f)
        fadd(r4, r4, 0.5).fmul(r3, r3, r3)      # (f (1 - f))^2: 0 at a line's middle, 1/16 between two
        fmax(r4, r4, 0).fmul(dk, r3, sc16)      # the beam's darkness here
        fmin(wf, r4, 1.0)           # line L + 1's share, sharpened
        mov(tmua, pa)               # line L's pixel,
        mov(tmua, pb)               # line L + 1's,
        mov(tmua, maddr, sig=thrsw) # and the column's mask factor
        add(caddr, caddr, c64)      # (delay slot)
        add(maddr, maddr, c64)      # (delay slot)
        nop(sig=ldtmu(va))
        nop(sig=ldtmu(vb))
        nop(sig=ldtmu(m))
        itof(r0, m)                 # the mask's factor, 0..256
        fmul(r1, r0, dk)
        fsub(r0, r0, r1)            # ... less the beam's darkness: the weights' whole
        fmul(r1, r0, wf)            # line L + 1's weight
        fsub(r0, r0, r1)            # line L's
        ftoiz(r0, r0)
        ftoiz(r1, r1)
        band(r2, va, mbr)           # the outer channels
        band(r3, vb, mbr).umul24(r2, r2, r0)
        band(r4, va, mg).umul24(r3, r3, r1)
        band(q, vb, mg).umul24(r4, r4, r0)     # (not r5: a write to it is a quad broadcast)
        add(r2, r2, r3).umul24(q, q, r1)
        add(r4, r4, q)
        shr(r2, r2, 8)
        shr(r4, r4, 8)
        band(r2, r2, mbr)
        band(r4, r4, mg)
        bor(r0, r2, r4)
        band(r0, r0, inm)           # off the curved picture: black
        bor(tmud, r0, ctop)         # alpha opaque
        mov(tmua, oaddr)            # the pixel written
        fadd(xf, xf, xstep)
        sub(blk, blk, 1, cond="pushz")
        block.b(cond="anyna")
        add(oaddr, oaddr, c64)      # (delay slot) the next block's pixel
        nop()                       # (delay slot)
        nop()                       # (delay slot)

    the_end(asm)


PROGRAMS = (
    ('v3d_lines_code', qpu_lines, {}),
    ('v3d_screen_code', qpu_screen, {}),
    ('v3d_lines_curve_code', qpu_lines_curve, {}),
    ('v3d_screen_curve_code', qpu_screen_curve, {}),
    ('v3d_lines_blend_code', qpu_lines, dict(blend=True)),
    ('v3d_lines_curve_blend_code', qpu_lines_curve, dict(blend=True)),
    ('v3d_lines_glow_code', qpu_lines, dict(glow=True)),
    ('v3d_lines_curve_glow_code', qpu_lines_curve, dict(glow=True)),
    ('v3d_lines_blend_glow_code', qpu_lines, dict(blend=True, glow=True)),
    ('v3d_lines_curve_blend_glow_code', qpu_lines_curve, dict(blend=True, glow=True)),
    ('v3d_glow_code', qpu_glow, {}),
    ('v3d_afterglow_code', qpu_afterglow, {}),
)

print('/* AGA-PISTORM - the CRT look\'s GPU programs (arm/aga_v3d.c), made by')
print(' * tools/v3d/mkcrt.py with py-videocore6\'s assembler (Idein Inc., MIT):')
print(' * RGB2RTG\'s passes for the core\'s 32-bit pixels. Do not edit: run')
print(' *     python3 tools/v3d/mkcrt.py > arm/aga_v3d_code.h')
print(' * SPDX-License-Identifier: GPL-2.0-only')
print(' */')
print('#ifndef AGA_V3D_CODE_H')
print('#define AGA_V3D_CODE_H')
print('#include <stdint.h>')
for name, f, kw in PROGRAMS:
    words = assemble(f, **kw)
    print('#define %s_N %d' % (name.upper(), len(words)))
    print('static const uint64_t %s[%d] = {' % (name, len(words)))
    for i in range(0, len(words), 3):
        print('    ' + ', '.join('0x%016xull' % w for w in words[i:i + 3]) + ',')
    print('};')
print('#endif')
