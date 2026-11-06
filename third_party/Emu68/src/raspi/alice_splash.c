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

/* ---- the animation: a USB card and a network card going into a desktop computer ----
   Drawn between the emblem and the header's text, in a scene of AN_LW x AN_LH units
   scaled by ak (1 to AN_KMAX), into abuf first and then copied in one go (no flicker).
   al_anim() draws the scene as it is ms milliseconds after the first call: the lid
   lifts, the USB card goes in, the network card, the lid closes, the power LED comes
   on. The cards' LEDs show the real state: green ready, amber waiting, red failed. */
#define AN_LW   180
#define AN_LH   88
#define AN_KMAX 3
#define AN_CAP  18                              /* the caption line under the scene */
static uint16_t abuf[AN_LW * AN_KMAX * AN_LH * AN_KMAX];
static uint32_t ax, ay, ak, acap;               /* where, how big (0: no room), caption or not */

static void anim_place(uint32_t x0, uint32_t x1)
{
    uint32_t aw = x1 > x0 ? x1 - x0 : 0, ah = hdr_h > 14 ? hdr_h - 14 : 0, k;
    ak = 0;
    for (k = AN_KMAX; k >= 1; k--)
        if (AN_LW * k <= aw && AN_LH * k <= ah) break;
    if (k < 1) return;
    acap = AN_LH * k + AN_CAP <= ah;
    ak = k;
    ax = x0 + (aw - AN_LW * k) / 2;
    ay = 6 + (ah - AN_LH * k - (acap ? AN_CAP : 0)) / 2;
}

/* a filled rectangle in scene units, clipped to the scene */
static void ar(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > AN_LW) w = AN_LW - x;
    if (y + h > AN_LH) h = AN_LH - y;
    if (w <= 0 || h <= 0) return;
    uint32_t bw = AN_LW * ak;
    uint16_t v = PX(c);
    for (uint32_t j = (uint32_t)y * ak; j < (uint32_t)(y + h) * ak; j++) {
        uint16_t *p = abuf + j * bw;
        for (uint32_t i = (uint32_t)x * ak; i < (uint32_t)(x + w) * ak; i++) p[i] = v;
    }
}

/* t from 0 to 1000 between a and b ms, eased out (fast, then settling) */
static int ease(uint32_t ms, uint32_t a, uint32_t b)
{
    if (ms <= a) return 0;
    if (ms >= b) return 1000;
    int u = (int)((ms - a) * 1000 / (b - a)), v = 1000 - u;
    return 1000 - v * v / 1000;
}

#define C_BEIGE   RGB(206, 196, 168)
#define C_BEIGE_D RGB(160, 150, 124)
#define C_INSIDE  RGB(24, 26, 32)
#define C_BOARD   RGB(30, 104, 58)
#define C_TRACE   RGB(64, 146, 88)
#define C_CHIP    RGB(16, 16, 18)
#define C_GOLD    RGB(212, 172, 60)
#define C_METAL   RGB(176, 180, 190)
#define C_USBPCB  RGB(36, 78, 170)
#define C_NETPCB  RGB(118, 58, 150)

static uint16_t led(int state, uint32_t ms)
{
    if (state == 1) return (ms / 300) % 4 ? C_OK : RGB(40, 90, 50);     /* ready: lit, a flicker of traffic */
    if (state == 2) return C_FAIL;
    return (ms / 500) % 2 ? C_WAIT : RGB(90, 70, 20);                  /* waiting: blinking amber */
}

/* a card standing in a slot whose left edge is x, lowered by drop (0 = in) */
static void card(int x, int drop, uint16_t pcb, int net, uint16_t ledc)
{
    int y = 47 - drop;
    ar(x, y, 22, 20, pcb);                       /* the board */
    ar(x + 1, y + 20, 20, 2, C_GOLD);            /* its edge connector */
    ar(x + 3, y + 4, 7, 6, C_CHIP);              /* chips */
    ar(x + 12, y + 5, 5, 4, C_CHIP);
    ar(x + 3, y + 13, 12, 1, C_TRACE);
    ar(x + 3, y + 15, 9, 1, C_TRACE);
    ar(x + 22, y - 2, 3, 24, C_METAL);           /* the bracket at the back */
    if (!net) {                                  /* USB: four sockets on the bracket */
        for (int i = 0; i < 4; i++) ar(x + 22, y + 1 + i * 5, 3, 3, C_CREAM);
    } else {                                     /* network: an RJ45 socket and the WiFi aerial */
        ar(x + 21, y + 3, 4, 5, C_CHIP);
        ar(x + 22, y + 4, 2, 2, C_GOLD);
        ar(x + 23, y - 14, 1, 12, C_DIM);
        ar(x + 22, y - 16, 3, 3, C_DIM);
    }
    ar(x + 2, y + 1, 3, 2, ledc);                /* its LED */
}

static int card_state_usb(const struct al_status *s)
{
    return s->usb_fail ? 2 : (s->stages & 1024u) ? 1 : 0;
}

static int card_state_net(const struct al_status *s)
{
    if (s->eth_col == 1 || s->wifi_col == 1) return 1;
    int eth = s->eth && s->eth[0], wifi = s->wifi && s->wifi[0];
    if ((eth && s->eth_col == 2) && (!wifi || s->wifi_col == 2)) return 2;
    if (!eth && wifi && s->wifi_col == 2) return 2;
    return 0;
}

void al_anim(uint32_t ms, const struct al_status *s)
{
    if (!fb || !ak) return;
    /* the timeline */
    int lid = ease(ms, 0, 700) - ease(ms, 3900, 4600);          /* 0 shut .. 1000 up */
    int usb = 1000 - ease(ms, 700, 1900), net = 1000 - ease(ms, 2300, 3500);
    int done = ms >= 4600;
    int us = card_state_usb(s), ns = card_state_net(s);

    ar(0, 0, AN_LW, AN_LH, C_BG);
    /* the case: front panel on the left, the inside cut away to show the board */
    ar(18, 40, 146, 44, C_BEIGE);
    ar(18, 82, 146, 2, C_BEIGE_D);
    ar(36, 44, 124, 36, C_INSIDE);
    ar(22, 50, 12, 2, C_CHIP);                   /* the floppy drive's slot */
    ar(22, 54, 4, 1, (done && (ms / 120) % 7 == 0) ? C_OK : C_BEIGE_D);   /* its light */
    ar(22, 74, 3, 3, done ? C_OK : C_BEIGE_D);   /* power */
    ar(160, 44, 4, 36, C_BEIGE_D);               /* the back, with its slot openings */
    ar(161, 47, 2, 20, C_INSIDE);
    ar(161, 52, 2, 20, C_INSIDE);
    /* the motherboard */
    ar(36, 72, 124, 6, C_BOARD);
    for (int i = 0; i < 6; i++) ar(40 + i * 18, 76, 10, 1, C_TRACE);
    ar(40, 66, 10, 6, C_CHIP);                   /* the CPU */
    ar(41, 67, 8, 1, C_DIM);
    ar(54, 68, 8, 4, C_CHIP);
    ar(130, 68, 6, 4, C_CHIP);
    ar(140, 68, 12, 4, C_CHIP);
    ar(70, 69, 22, 3, C_CHIP);                   /* the two slots */
    ar(104, 69, 22, 3, C_CHIP);
    /* the cards: falling, then in */
    card(70, (65 + 47) * usb / 1000, C_USBPCB, 0, (usb ? C_BEIGE_D : led(us, ms)));
    card(104, (65 + 47) * net / 1000, C_NETPCB, 1, (net ? C_BEIGE_D : led(ns, ms)));
    if (done && ns == 1 && (ms / 400) % 3) {     /* the aerial at work */
        ar(130, 28, 1, 3, C_CYAN);
        ar(132, 26, 1, 7, C_CYAN);
    }
    /* the lid: on the case, or lifted */
    {
        int up = 30 * lid / 1000;
        ar(16, 36 - up, 150, 5, C_BEIGE);
        ar(16, 41 - up, 150, 1, C_BEIGE_D);
        if (lid == 0) ar(36, 42, 124, 2, C_BEIGE);
    }
    /* then the monitor comes down onto it; its screen lights up once both cards are ready */
    if (ms >= 4600) {
        int my = -40 + 41 * ease(ms, 4600, 5300) / 1000;
        int lit = ms >= 5300 && us == 1 && ns == 1;
        ar(60, my, 64, 32, C_BEIGE);                 /* the bezel */
        ar(60, my + 30, 64, 2, C_BEIGE_D);
        ar(86, my + 32, 12, 3, C_BEIGE_D);           /* its foot */
        ar(64, my + 3, 56, 25, lit ? RGB(0, 84, 170) : RGB(14, 16, 22));
        if (lit) {                                   /* a desktop: a title bar and two windows */
            ar(64, my + 3, 56, 3, C_CREAM);
            ar(70, my + 10, 22, 12, C_CREAM);
            ar(71, my + 11, 20, 2, C_RUFOUS);
            ar(96, my + 13, 18, 10, C_CREAM);
            ar(97, my + 14, 16, 2, C_SLATE);
        }
        ar(114, my + 29, 3, 1, lit ? C_OK : C_DIM);  /* its power light */
    }
    /* to the screen in one go */
    uint32_t bw = AN_LW * ak, bh = AN_LH * ak;
    for (uint32_t j = 0; j < bh && ay + j < fh; j++) {
        uint16_t *d = fb + (ay + j) * pitch + ax, *q = abuf + j * bw;
        for (uint32_t i = 0; i < bw; i++) d[i] = q[i];
    }
    if (acap) {
        const char *cap = ms < 700 ? "opening the case" : ms < 2300 ? "fitting the USB card" :
                          ms < 3900 ? "fitting the network card" : ms < 4600 ? "closing the case" :
                          (us == 1 && ns == 1) ? "USB and network ready" : us == 1 ? "USB ready, waiting for the network" :
                          "starting";
        uint32_t n = slen(cap), cy = ay + bh + 2;
        rect(ax, cy, bw, 16, C_BG);
        text(ax + (bw > n * 8 ? (bw - n * 8) / 2 : 0), cy, cap, C_DIM, n);
    }
}

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
    uint32_t y = (hdr_h > 66 ? (hdr_h - 60) / 2 : 4), text_x = fw;
    for (int i = 0; i < 3; i++) {
        if (!l[i]) continue;
        uint32_t n = slen(l[i]);
        uint32_t x = fw > n * 8 + 20 ? fw - n * 8 - 20 : 0;
        if (x < right_x) continue;                   /* no room beside the logo */
        text(x, y + (uint32_t)i * 20, l[i], c[i], n);
        if (x < text_x) text_x = x;
    }
    rect(0, hdr_h - 2, fw, 1, C_RULE);
    anim_place(right_x, text_x > 24 ? text_x - 24 : 0);
}

static uint32_t chip(uint32_t x, const char *label, const char *state, uint16_t col)
{
    uint32_t y = strip_y + 8;
    rect(x, y + 3, 10, 10, col);
    x = text(x + 16, y, label, C_TEXT, 16) + 8;
    x = text(x, y, state, col, 48);
    return x + 28;
}

static unsigned num(char *b, unsigned k, int v)
{
    char t[12];
    unsigned n = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < sizeof t);
    while (n) b[k++] = t[--n];
    return k;
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
        int narrow = fw < 1600;                          /* room for the WiFi too */
        b[k++] = (char)('0' + (kb % 10)); for (const char *t = narrow ? " kbd" : " keyboard"; *t; ) b[k++] = *t++;
        if (kb != 1 && !narrow) b[k++] = 's';
        b[k++] = ' '; b[k++] = ' ';
        b[k++] = (char)('0' + (ms % 10)); for (const char *t = " mouse"; *t; ) b[k++] = *t++;
        if (ms != 1) { k -= 3; for (const char *t = "ice"; *t; ) b[k++] = *t++; }
        b[k] = 0;
        x = chip(x, "INPUT", b, (kb || ms) ? C_OK : C_DIM);
    }
    if (s->hdf_boot >= 0) {                              /* from alice68hdf.device's summary line */
        char b[64];
        unsigned k = 0;
        int nb = s->hdf_boot, no = s->hdf_other, nl = s->hdf_left;
        if (!nb && !no && !nl) {
            for (const char *t = "none"; *t; ) b[k++] = *t++;
        } else {
            int narrow = fw < 1600;
            k = num(b, k, nb); for (const char *t = narrow ? " boot  " : " bootable  "; *t; ) b[k++] = *t++;
            k = num(b, k, no); for (const char *t = narrow ? " other" : " non-bootable"; *t; ) b[k++] = *t++;
            if (nl) { b[k++] = ' '; b[k++] = ' '; k = num(b, k, nl); for (const char *t = " left out"; *t; ) b[k++] = *t++; }
        }
        b[k] = 0;
        x = chip(x, "HARDFILES", b, nl ? C_WAIT : (nb || no) ? C_OK : C_DIM);
    }
    if (s->eth && s->eth[0] && x + 16 * 8 < fw)          /* the kernel's Ethernet (alice_net.c) */
        x = chip(x, "LAN", s->eth, s->eth_col == 1 ? C_OK : s->eth_col == 2 ? C_FAIL : C_WAIT);
    if (s->wifi && s->wifi[0] && x + 16 * 8 < fw) {      /* the kernel's WiFi (alice_net.c), cut to the room left */
        char w[48];
        uint32_t room = (fw - x - 16 - 5 * 8 - 16) / 8, k = 0;
        for (; s->wifi[k] && k < room && k < sizeof w - 1; k++) w[k] = s->wifi[k];
        w[k] = 0;
        chip(x, "WIFI", w, s->wifi_col == 1 ? C_OK : s->wifi_col == 2 ? C_FAIL : C_WAIT);
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
