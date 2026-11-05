/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 boot splash - see alice_splash.h.
 *
 *   +--------------------------------------------------------------------+
 *   | [emblem] KESTREL68                       Alice68 0.1.0            |
 *   |          standalone AGA - Raspberry Pi 4 Emu68 ... / display WxH   |
 *   |--------------------------------------------------------------------|
 *   | VIDEO * ROM * CPU1 * USB-A * INPUT 1 kbd 1 mouse * 68K              |
 *   |--------------------------------+-----------------------------------|
 *   | SYSTEM                         | USB                               |
 *   | kernel log ...                 | [FRF-XHCI] lines ...              |
 *   |                                |                                   |
 *   |--------------------------------------------------------------------|
 *   | built on Emu68 - AGA-PiStorm - EMU68AGA Bare Metal - ...  GPL-2.0  |
 *   +--------------------------------------------------------------------+
 *
 * Pixels are RGB565, little-endian in memory whatever the CPU's order.
 */
#include "alice_splash.h"
#include "logo/logo_alice.h"

extern const uint8_t  topaz8_chardata[];
extern const uint32_t topaz8_charloc[];

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PX(c) ((uint16_t)__builtin_bswap16((uint16_t)(c)))
#else
#define PX(c) ((uint16_t)(c))
#endif

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BG      RGB(0, 0, 0)
#define C_PANEL   RGB(10, 13, 22)
#define C_RULE    RGB(40, 52, 76)
#define C_RUFOUS  RGB(214, 110, 42)
#define C_SLATE   RGB(104, 132, 168)
#define C_CREAM   RGB(240, 230, 210)
#define C_TEXT    RGB(196, 200, 210)
#define C_DIM     RGB(120, 128, 144)
#define C_OK      RGB(96, 200, 120)
#define C_WAIT    RGB(230, 180, 60)
#define C_FAIL    RGB(240, 70, 60)
#define C_CYAN    RGB(90, 200, 220)

static uint16_t *fb;
static uint32_t pitch, fw, fh;          /* pitch in pixels */
static uint32_t scale, hdr_h, strip_y, pan_y, pan_h, foot_y;

static uint32_t slen(const char *s) { uint32_t n = 0; while (s && s[n]) n++; return n; }

static int has(const char *l, const char *w)
{
    for (; *l; l++) {
        const char *a = l, *b = w;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    if (!fb || x >= fw || y >= fh) return;
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    uint16_t v = PX(c);
    for (uint32_t j = 0; j < h; j++) {
        uint16_t *p = fb + (y + j) * pitch + x;
        for (uint32_t i = 0; i < w; i++) p[i] = v;
    }
}

static void glyph(uint32_t x, uint32_t y, uint8_t c, uint16_t col)
{
    if (c < 32 || c > 126 || x + 8 > fw || y + 16 > fh) return;
    const uint8_t *d = &topaz8_chardata[(topaz8_charloc[c - 32] >> 16) >> 3];
    uint16_t v = PX(col);
    for (uint32_t yy = 0; yy < 16; yy++) {
        uint8_t bits = *d;
        uint16_t *p = fb + (y + yy) * pitch + x;
        for (uint32_t xx = 0; xx < 8; xx++) if (bits & (0x80u >> xx)) p[xx] = v;
        if (yy & 1) d += 192;                    /* Topaz 8 is 8 lines, doubled */
    }
}

static uint32_t text(uint32_t x, uint32_t y, const char *s, uint16_t col, uint32_t maxc)
{
    uint32_t n = 0;
    for (; s && *s && n < maxc; s++, n++, x += 8) glyph(x, y, (uint8_t)*s, col);
    return x;
}

void al_setup(uint16_t *f, uint32_t pitch_bytes, uint32_t w, uint32_t h)
{
    fb = f; pitch = pitch_bytes / 2; fw = w; fh = h;
    scale = (w >= 1600 && h >= 900) ? 2 : 1;
    hdr_h = LOGO_ALICE_HEIGHT * scale + 20;
    if (w < LOGO_ALICE_WIDTH + 16) hdr_h = 52;
    strip_y = hdr_h;
    pan_y = strip_y + 34;
    foot_y = h - 22;
    pan_h = foot_y > pan_y + 60 ? foot_y - 6 - pan_y : 0;
}

void al_clear(void) { rect(0, 0, fw, fh, C_BG); }

static void logo(uint32_t x0, uint32_t y0)
{
    uint32_t x = 0, y = 0;
    for (uint32_t i = 0; i + 1 < LOGO_ALICE_LEN; i += 2) {
        uint32_t n = logo_alice[i];
        uint16_t c = PX(logo_alice_pal[logo_alice[i + 1]]);
        while (n--) {
            for (uint32_t dy = 0; dy < scale; dy++) {
                uint16_t *p = fb + (y0 + y * scale + dy) * pitch + x0 + x * scale;
                for (uint32_t dx = 0; dx < scale; dx++) p[dx] = c;
            }
            if (++x >= LOGO_ALICE_WIDTH) { x = 0; y++; }
        }
    }
}

void al_header(const char *l1, const char *l2, const char *l3)
{
    if (!fb) return;
    rect(0, 0, fw, hdr_h, C_BG);
    uint32_t right_x = 16;
    if (fw >= LOGO_ALICE_WIDTH + 16) {
        logo(12, 10);
        right_x = 12 + LOGO_ALICE_WIDTH * scale + 24;
    } else {
        text(12, 18, ALICE_NAME, C_CREAM, 40);
    }
    const char *l[3] = { l1, l2, l3 };
    const uint16_t c[3] = { C_CREAM, C_TEXT, C_DIM };
    uint32_t y = (hdr_h > 66 ? (hdr_h - 60) / 2 : 4);
    for (int i = 0; i < 3; i++) {
        if (!l[i]) continue;
        uint32_t n = slen(l[i]);
        uint32_t x = fw > n * 8 + 20 ? fw - n * 8 - 20 : 0;
        if (x < right_x) continue;                   /* no room beside the logo */
        text(x, y + (uint32_t)i * 20, l[i], c[i], n);
    }
    rect(0, hdr_h - 2, fw, 1, C_RULE);
}

static uint32_t chip(uint32_t x, const char *label, const char *state, uint16_t col)
{
    uint32_t y = strip_y + 8;
    rect(x, y + 3, 10, 10, col);
    x = text(x + 16, y, label, C_TEXT, 16) + 8;
    x = text(x, y, state, col, 24);
    return x + 28;
}

void al_status_draw(const struct al_status *s)
{
    if (!fb) return;
    rect(0, strip_y, fw, 32, C_BG);
    uint32_t st = s->stages, x = 14;
    x = chip(x, "VIDEO", (st & 2u) ? "ready" : "wait", (st & 2u) ? C_OK : C_WAIT);
    x = chip(x, "ROM", (st & 4u) ? "loaded" : "wait", (st & 4u) ? C_OK : C_WAIT);
    x = chip(x, "68K", (st & 32u) ? "running" : "wait", (st & 32u) ? C_OK : C_WAIT);
    x = chip(x, "CPU1", (st & 64u) ? "up" : "wait", (st & 64u) ? C_OK : C_WAIT);
    const char *u = s->usb_fail ? "FAILED" : (st & 1024u) ? "ready" : (st & 256u) ? "starting" : "wait";
    x = chip(x, "USB", u, s->usb_fail ? C_FAIL : (st & 1024u) ? C_OK : C_WAIT);
    if (s->keyboards >= 0 || s->mice >= 0) {
        char b[40];
        unsigned k = 0;
        int kb = s->keyboards < 0 ? 0 : s->keyboards, ms = s->mice < 0 ? 0 : s->mice;
        b[k++] = (char)('0' + (kb % 10)); for (const char *t = " keyboard"; *t; ) b[k++] = *t++;
        if (kb != 1) b[k++] = 's';
        b[k++] = ' '; b[k++] = ' ';
        b[k++] = (char)('0' + (ms % 10)); for (const char *t = " mouse"; *t; ) b[k++] = *t++;
        if (ms != 1) { k -= 3; for (const char *t = "ice"; *t; ) b[k++] = *t++; }
        b[k] = 0;
        chip(x, "INPUT", b, (kb || ms) ? C_OK : C_DIM);
    }
    rect(0, strip_y + 31, fw, 1, C_RULE);
}

void al_footer(void)
{
    if (!fb) return;
    static const char *const long_c =
        ALICE_NAME " is built on Emu68 (Michal Schulz), AGA-PiStorm (Astair86), "
        "EMU68AGA Bare Metal (Future Retro Fusion), UAE/WinUAE and TinyUSB  -  GPL-2.0";
    static const char *const short_c = "Emu68 - AGA-PiStorm - FRF - UAE - TinyUSB  -  GPL-2.0";
    const char *t = slen(long_c) * 8 + 24 <= fw ? long_c : short_c;
    uint32_t n = slen(t);
    rect(0, foot_y - 2, fw, fh - foot_y + 2, C_BG);
    rect(0, foot_y - 3, fw, 1, C_RULE);
    text(fw > n * 8 ? (fw - n * 8) / 2 : 0, foot_y + 2, t, C_DIM, n);
}

static int two_panels(void) { return fw >= 1000; }

static void pan_geom(int which, uint32_t *x, uint32_t *w)
{
    if (!two_panels()) { *x = 8; *w = which ? 0 : fw - 16; return; }
    uint32_t half = (fw - 24) / 2;
    *x = which ? 16 + half : 8;
    *w = half;
}

unsigned al_panel_cols(int which)
{
    uint32_t x, w;
    pan_geom(which, &x, &w);
    return w > 16 ? (w - 16) / 8 : 0;
}

static uint16_t line_colour(int which, const char *l)
{
    if (has(l, "FAIL") || has(l, "ERROR") || has(l, "rror") || has(l, "failed")) return C_FAIL;
    if (which) {
        if (has(l, "ready") || has(l, "-> Neptune") || has(l, "running") || has(l, "link up")) return C_OK;
        if (has(l, "eject") || has(l, "mode switch")) return C_CYAN;
        return C_TEXT;
    }
    if (has(l, "[alice68hdf]"))                       /* hardfiles: what was mounted, and what not */
        return (has(l, "not mounted") || has(l, "left out") || has(l, "ignored")) ? C_WAIT : C_CYAN;
    if (has(l, "[STAGE")) return C_RUFOUS;
    if (has(l, "[FRF-XHCI]") || has(l, "[FRF-USB]") || has(l, "TINYUSB")) return C_SLATE;
    if (has(l, "[AGA]")) return C_CYAN;
    return C_TEXT;
}

int al_panel(int which, const char *const *lines, unsigned n)
{
    uint32_t x, w;
    pan_geom(which, &x, &w);
    if (!fb || !w || pan_h < 60) return 0;
    rect(x, pan_y, w, pan_h, C_PANEL);
    rect(x, pan_y, w, 20, RGB(18, 24, 40));
    rect(x, pan_y + 20, w, 1, which ? C_SLATE : C_RUFOUS);
    text(x + 8, pan_y + 2, which ? "USB" : "SYSTEM", which ? C_SLATE : C_RUFOUS, 8);
    uint32_t rows = (pan_h - 28) / 16, cols = (w - 16) / 8;
    unsigned first = n > rows ? n - rows : 0;
    for (unsigned i = first, r = 0; i < n; i++, r++)
        text(x + 8, pan_y + 26 + r * 16, lines[i], line_colour(which, lines[i]), cols);
    return 1;
}
