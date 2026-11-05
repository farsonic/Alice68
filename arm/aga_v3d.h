/* AGA-PISTORM - the CRT look on the Pi 4's 3D GPU (aga_v3d.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_V3D_H
#define AGA_V3D_H

#include <stdint.h>

/* start.c, at boot: hand over the GPU room - the two pictures the passes
   draw into, identity-mapped and uncached - or none at all (a Pi 3) */
void aga_v3d_room(uint8_t *va, uintptr_t pa, uint32_t bytes);

/* core 0, the control words: 0 Curved screen (0 flat, 1-9), 1 Glow (0 off,
   1-100%), 2 Afterglow (0 off, 1-3 light .. strong). The GPU wakes the
   first time one of the damn things is on. Returns aga_v3d_wanted(). */
int  aga_v3d_set(int what, int v);
int  aga_v3d_wanted(void);              /* one of them is on and the GPU can draw */
int  aga_v3d_ready(void);               /* the GPU woken, its programs made */

/* core 3: pass B's tables for a picture ow x oh from sh lines at vstep
   (16.16), look 0 Sharp 1 Smooth - each bloody HDMI row's scanline darkness
   (0..255, NULL none; flip: read the rows bottom up) and each column's CRT
   mask darkness (NULL none). 1 made. */
int  aga_v3d_tables(const uint8_t *row_dark, int flip, const uint8_t *col_dark, uint32_t ow, uint32_t oh,
                    uint32_t sh, uint32_t vstep, int look);

/* a frame to draw: its crop at src (physical; the core's 32-bit words), the
   frame before's at prev (same place, another page, 0 none: Frame blend),
   drawn ow x oh - the tables' size */
struct aga_v3d_frame {
    uint32_t src, pitch, prev;
    uint32_t sw, sh;                    /* the crop: pixels, lines */
    uint32_t ow, oh;                    /* the picture on HDMI */
    uint32_t stepx, stepy;              /* source pixels an output one, 16.16 */
    int look;                           /* 0 Sharp, 1 Smooth */
    int scan_pct;                       /* the scanlines' strength, a curved screen's beam */
};
int  aga_v3d_start(const struct aga_v3d_frame *f, int slot);   /* core 3: 1 sent */
int  aga_v3d_poll(int *slot);           /* core 3: 1 the picture is whole in *slot, -1 failed, 0 else */
int  aga_v3d_busy(void);
uint32_t aga_v3d_slot(int s, uint32_t *pitch, uint32_t ow);   /* a picture's physical address, its row */
uint32_t aga_v3d_need_us(void);         /* a draw's time lately: up at once, slowly down */

#endif
