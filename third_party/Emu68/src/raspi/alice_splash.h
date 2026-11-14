/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 boot splash: header, status bar, SYSTEM and USB log panels, footer.
 * Self-contained (framebuffer, Topaz glyphs, the generated emblem), so
 * tools/splash-preview can build it on a PC and render the exact screen the
 * Pi shows. alice_setup.c draws the setup screen with the same primitives.
 */
#ifndef ALICE_SPLASH_H
#define ALICE_SPLASH_H
#include <stdint.h>

#define ALICE_NAME    "Alice68"
#define ALICE_VERSION "0.5.0"

/* stage bits as frf_boot_stage() numbers them */
struct al_status {
    uint32_t stages;            /* bit n: stage n reached */
    int keyboards, mice;        /* -1: not known yet */
    int usb_fail;               /* the USB-A host gave up */
    int hdf_boot, hdf_other, hdf_left;  /* hardfiles: bootable, mounted not bootable, left out; -1 not known yet */
    const char *wifi;           /* the WiFi's state in a few words; 0 = no WiFi chip/driver (yet) */
    int wifi_col;               /* 0 waiting, 1 good, 2 failed */
    const char *eth;            /* the kernel's Ethernet, as the WiFi; 0 or "" = not in use */
    int eth_col;
};

/* a line of the header's table: "Version   0.5.0" */
struct al_info { const char *label, *value; };

void al_setup(uint16_t *fb, uint32_t pitch_bytes, uint32_t w, uint32_t h);
void al_clear(void);
void al_header(const struct al_info *rows, unsigned n);
void al_status_draw(const struct al_status *s);
/* the footer: credits, and at the right a key and what it does ("DEL", "Setup"), or none */
void al_footer(const char *key, const char *what);
/* which: 0 SYSTEM (left), 1 USB (right). lines[0..n-1], oldest first; the
   panel shows the newest that fit. Returns 0 if the panel has no room. */
int  al_panel(int which, const char *const *lines, unsigned n);
unsigned al_panel_cols(int which);

/* ---- drawing, for alice_setup.c --------------------------------------------- */
#define AL_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define AL_BG      AL_RGB(8, 8, 16)         /* the page */
#define AL_PANEL   AL_RGB(16, 20, 30)
#define AL_TITLE   AL_RGB(24, 30, 44)       /* panel title bars */
#define AL_RULE    AL_RGB(44, 52, 70)
#define AL_TEXT    AL_RGB(200, 204, 212)
#define AL_BRIGHT  AL_RGB(240, 236, 228)
#define AL_DIM     AL_RGB(116, 124, 140)
#define AL_ACCENT  AL_RGB(214, 110, 42)     /* the emblem's rufous */
#define AL_SEL     AL_RGB(46, 70, 112)      /* a selected row */
#define AL_OK      AL_RGB(112, 190, 124)
#define AL_WAIT    AL_RGB(222, 172, 72)
#define AL_FAIL    AL_RGB(228, 84, 72)
#define AL_FONT_W  8u
#define AL_FONT_H  16u
uint32_t al_width(void);
uint32_t al_height(void);
void     al_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c);
void     al_frame(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c);   /* a 1-pixel outline */
/* text at x, y (top left), at most maxc characters; returns the x after it */
uint32_t al_text(uint32_t x, uint32_t y, const char *s, uint16_t c, uint32_t maxc);
/* a key as on a keycap: an outlined box around the name; returns the x after it */
uint32_t al_key(uint32_t x, uint32_t y, const char *name);
/* the emblem at x, y (LOGO_ALICE_WIDTH x LOGO_ALICE_HEIGHT) */
void     al_emblem(uint32_t x, uint32_t y);
#endif
