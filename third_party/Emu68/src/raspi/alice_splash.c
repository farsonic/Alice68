/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 boot splash - see alice_splash.h.
 *
 *   +----------------------------------------------------------------------+
 *   |  [emblem] ALICE68                               Version   0.5.0       |
 *   |           standalone AGA - Raspberry Pi 4       Build     6be46ab ... |
 *   |                                                 Emu68     1.1.0 ...   |
 *   |----------------------------------------------------------------------|
 *   | | VIDEO   | KICKSTART | 68K     | USB     | INPUT   | STORAGE | NET.. |
 *   | | ready   | loaded    | running | ready   | 1 kbd.. | 1 boot  | ...   |
 *   |----------------------------------------------------------------------|
 *   | SYSTEM                          | USB                                |
 *   | BOOT  kernel log ...            | port 1: ...                        |
 *   | HDF   ...                       |                                    |
 *   |----------------------------------------------------------------------|
 *   | built on Emu68, AGA-PiStorm, ...                          [DEL] Setup |
 *   +----------------------------------------------------------------------+
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

#define MARGIN   24u                    /* the page's left and right margins */
#define CELL_H   56u                    /* the status bar */
#define FOOT_H   32u

static uint16_t *fb;
static uint32_t pitch, fw, fh;          /* pitch in pixels */
static uint32_t hdr_h, bar_y, pan_y, pan_h, foot_y;

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

uint32_t al_width(void) { return fw; }
uint32_t al_height(void) { return fh; }

void al_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
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

void al_frame(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    if (w < 2 || h < 2) return;
    al_fill(x, y, w, 1, c);
    al_fill(x, y + h - 1, w, 1, c);
    al_fill(x, y, 1, h, c);
    al_fill(x + w - 1, y, 1, h, c);
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

uint32_t al_text(uint32_t x, uint32_t y, const char *s, uint16_t col, uint32_t maxc)
{
    if (!fb) return x;
    uint32_t n = 0;
    for (; s && *s && n < maxc; s++, n++, x += 8) glyph(x, y, (uint8_t)*s, col);
    return x;
}

uint32_t al_key(uint32_t x, uint32_t y, const char *name)
{
    uint32_t w = slen(name) * 8 + 12;
    al_fill(x, y - 3, w, 22, AL_TITLE);
    al_frame(x, y - 3, w, 22, AL_RULE);
    al_text(x + 6, y, name, AL_BRIGHT, 16);
    return x + w;
}

void al_emblem(uint32_t x0, uint32_t y0)
{
    if (!fb || x0 + LOGO_ALICE_WIDTH > fw || y0 + LOGO_ALICE_HEIGHT > fh) return;
    uint32_t x = 0, y = 0;
    for (uint32_t i = 0; i + 1 < LOGO_ALICE_LEN; i += 2) {
        uint32_t n = logo_alice[i];
        uint16_t c = PX(logo_alice_pal[logo_alice[i + 1]]);
        while (n--) {
            fb[(y0 + y) * pitch + x0 + x] = c;
            if (++x >= LOGO_ALICE_WIDTH) { x = 0; y++; }
        }
    }
}

void al_setup(uint16_t *f, uint32_t pitch_bytes, uint32_t w, uint32_t h)
{
    fb = f; pitch = pitch_bytes / 2; fw = w; fh = h;
    hdr_h = (w >= LOGO_ALICE_WIDTH + 2 * MARGIN && h >= 480) ? LOGO_ALICE_HEIGHT + 32 : 56;
    bar_y = hdr_h;
    pan_y = bar_y + CELL_H + 12;
    foot_y = h > FOOT_H ? h - FOOT_H : 0;
    pan_h = foot_y > pan_y + 72 ? foot_y - 12 - pan_y : 0;
}

void al_clear(void) { al_fill(0, 0, fw, fh, AL_BG); }

/* ---- header: the emblem, and a table of versions at the right ---------------- */
void al_header(const struct al_info *rows, unsigned n)
{
    if (!fb) return;
    al_fill(0, 0, fw, hdr_h, AL_BG);
    uint32_t left = MARGIN;
    if (hdr_h > 56) {
        al_emblem(MARGIN, 16);
        left = MARGIN + LOGO_ALICE_WIDTH + 32;
    } else {
        al_text(MARGIN, 20, ALICE_NAME " " ALICE_VERSION, AL_BRIGHT, 40);
        left = MARGIN + 16 * 8;
    }
    uint32_t lw = 0, vw = 0;
    for (unsigned i = 0; i < n; i++) {
        if (slen(rows[i].label) > lw) lw = slen(rows[i].label);
        if (slen(rows[i].value) > vw) vw = slen(rows[i].value);
    }
    uint32_t rows_fit = hdr_h > 56 ? (hdr_h - 16) / 20 : 0;
    uint32_t tw = (lw + 3 + vw) * 8;
    if (rows_fit && fw > tw + MARGIN && fw - tw - MARGIN >= left) {
        uint32_t shown = n < rows_fit ? n : rows_fit;
        uint32_t x = fw - MARGIN - tw, y = (hdr_h - shown * 20) / 2 + 2;
        for (unsigned i = 0; i < shown; i++, y += 20) {
            al_text(x, y, rows[i].label, AL_DIM, lw);
            al_text(x + (lw + 3) * 8, y, rows[i].value, i ? AL_TEXT : AL_BRIGHT, vw);
        }
    }
    al_fill(0, hdr_h - 1, fw, 1, AL_RULE);
}

/* ---- the status bar: one cell per part, a coloured edge for its state -------- */
static unsigned num(char *b, unsigned k, int v)
{
    char t[12];
    unsigned n = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < sizeof t);
    while (n) b[k++] = t[--n];
    return k;
}
static unsigned put(char *b, unsigned k, const char *s) { while (*s) b[k++] = *s++; return k; }

struct cell { const char *label; const char *value; uint16_t col; };

void al_status_draw(const struct al_status *s)
{
    if (!fb) return;
    struct cell c[8];
    unsigned n = 0;
    uint32_t st = s->stages;
    char in[48], hd[64];
    c[n++] = (struct cell){ "VIDEO", (st & 2u) ? "ready" : "starting", (st & 2u) ? AL_OK : AL_WAIT };
    c[n++] = (struct cell){ "KICKSTART", (st & 4u) ? "loaded" : "waiting", (st & 4u) ? AL_OK : AL_WAIT };
    c[n++] = (struct cell){ "68K", (st & 32u) ? "running" : "waiting", (st & 32u) ? AL_OK : AL_WAIT };
    c[n++] = (struct cell){ "USB", s->usb_fail ? "failed" : (st & 1024u) ? "ready" : (st & 256u) ? "starting" : "waiting",
                            s->usb_fail ? AL_FAIL : (st & 1024u) ? AL_OK : AL_WAIT };
    if (s->keyboards >= 0 || s->mice >= 0) {
        int kb = s->keyboards < 0 ? 0 : s->keyboards, ms = s->mice < 0 ? 0 : s->mice;
        unsigned k = num(in, 0, kb);
        k = put(in, k, kb == 1 ? " keyboard, " : " keyboards, ");
        k = num(in, k, ms);
        k = put(in, k, ms == 1 ? " mouse" : " mice");
        in[k] = 0;
        c[n++] = (struct cell){ "INPUT", in, (kb || ms) ? AL_OK : AL_DIM };
    } else {
        c[n++] = (struct cell){ "INPUT", "waiting", AL_WAIT };
    }
    if (s->hdf_boot >= 0) {                              /* from alice68hdf.device's summary line */
        unsigned k = 0;
        int nb = s->hdf_boot, no = s->hdf_other, nl = s->hdf_left;
        if (!nb && !no && !nl) k = put(hd, k, "no hardfiles");
        else {
            k = num(hd, k, nb); k = put(hd, k, " boot, ");
            k = num(hd, k, no); k = put(hd, k, " other");
            if (nl) { k = put(hd, k, ", "); k = num(hd, k, nl); k = put(hd, k, " left out"); }
        }
        hd[k] = 0;
        c[n++] = (struct cell){ "HARDFILES", hd, nl ? AL_WAIT : (nb || no) ? AL_OK : AL_DIM };
    } else {
        c[n++] = (struct cell){ "HARDFILES", (st & 32u) ? "scanning" : "waiting", AL_WAIT };
    }
    if (s->eth && s->eth[0])                             /* the kernel's Ethernet (alice_net.c) */
        c[n++] = (struct cell){ "ETHERNET", s->eth, s->eth_col == 1 ? AL_OK : s->eth_col == 2 ? AL_FAIL : AL_WAIT };
    if (s->wifi && s->wifi[0])                           /* the kernel's WiFi */
        c[n++] = (struct cell){ "WIFI", s->wifi, s->wifi_col == 1 ? AL_OK : s->wifi_col == 2 ? AL_FAIL : AL_WAIT };

    al_fill(0, bar_y, fw, CELL_H, AL_BG);
    uint32_t avail = fw > 2 * MARGIN ? fw - 2 * MARGIN : fw, x = MARGIN, need = 0, cw[8];
    for (unsigned i = 0; i < n; i++) {                 /* each as wide as its words, and a gap */
        uint32_t k = slen(c[i].label) > slen(c[i].value) ? slen(c[i].label) : slen(c[i].value);
        cw[i] = 14 + k * 8 + 32;
        if (cw[i] < 136) cw[i] = 136;
        need += cw[i];
    }
    if (need > avail)                                  /* too narrow: everyone gives up a share */
        for (unsigned i = 0; i < n; i++) cw[i] = cw[i] * avail / need;
    for (unsigned i = 0; i < n; i++) {
        uint32_t cols = cw[i] > 28 ? (cw[i] - 28) / 8 : 0;
        al_fill(x, bar_y + 12, 3, CELL_H - 24, c[i].col);
        al_text(x + 14, bar_y + 9, c[i].label, AL_DIM, cols);
        al_text(x + 14, bar_y + 29, c[i].value, AL_TEXT, cols);
        x += cw[i];
    }
    al_fill(0, bar_y + CELL_H - 1, fw, 1, AL_RULE);
}

/* ---- footer ----------------------------------------------------------------- */
void al_footer(const char *key, const char *what)
{
    if (!fb || !foot_y) return;
    static const char *const long_c =
        ALICE_NAME " is built on Emu68, AGA-PiStorm, EMU68AGA Bare Metal, UAE/WinUAE and TinyUSB. GPL-2.0";
    static const char *const short_c = "Emu68, AGA-PiStorm, EMU68AGA, UAE, TinyUSB. GPL-2.0";
    al_fill(0, foot_y, fw, FOOT_H, AL_BG);
    al_fill(0, foot_y, fw, 1, AL_RULE);
    uint32_t right = fw - MARGIN;
    if (key) {
        uint32_t ww = (slen(what) + 1) * 8, kw = slen(key) * 8 + 12;
        uint32_t x = right - ww - kw;
        al_key(x, foot_y + 9, key);
        al_text(x + kw + 8, foot_y + 9, what, AL_TEXT, 32);
        right = x - 24;
    }
    const char *t = MARGIN + slen(long_c) * 8 <= right ? long_c : short_c;
    al_text(MARGIN, foot_y + 9, t, AL_DIM, right > MARGIN ? (right - MARGIN) / 8 : 0);
}

/* ---- the log panels ---------------------------------------------------------- */
static int two_panels(void) { return fw >= 1000; }

static void pan_geom(int which, uint32_t *x, uint32_t *w)
{
    uint32_t inner = fw > 2 * MARGIN ? fw - 2 * MARGIN : fw;
    if (!two_panels()) { *x = MARGIN; *w = which ? 0 : inner; return; }
    uint32_t half = (inner - 16) / 2;
    *x = which ? MARGIN + half + 16 : MARGIN;
    *w = half;
}

#define TAG_COLS 6u                     /* "WIFI  " */

unsigned al_panel_cols(int which)
{
    uint32_t x, w;
    pan_geom(which, &x, &w);
    return w > 24 ? (w - 24) / 8 : 0;
}

/* "[alice68hdf] text" -> tag "HDF", the text; "[STAGE 3] what" -> "BOOT", "stage 3: what" */
static const char *split_tag(const char *l, char *tag, char *pre)
{
    tag[0] = pre[0] = 0;
    if (l[0] != '[') return l;
    const char *e = l + 1;
    while (*e && *e != ']' && e - l < 24) e++;
    if (*e != ']') return l;
    unsigned n = (unsigned)(e - l - 1);
    const char *t = l + 1, *rest = e[1] == ' ' ? e + 2 : e + 1;
    static const struct { const char *in, *out; } map[] = {
        { "alice68hdf", "HDF" }, { "alice68", "AMIGA" }, { "FRF-XHCI", "USB" }, { "FRF-USB", "USB" },
        { "FRFNET", "NET" }, { "TINYUSB", "USB" },
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        unsigned k = 0;
        while (map[i].in[k] && k < n && map[i].in[k] == t[k]) k++;
        if (!map[i].in[k] && k == n) { unsigned j = 0; for (; map[i].out[j]; j++) tag[j] = map[i].out[j]; tag[j] = 0; return rest; }
    }
    if (n > 6 && t[0] == 'S' && t[1] == 'T' && t[2] == 'A' && t[3] == 'G' && t[4] == 'E' && t[5] == ' ') {
        unsigned j = 0;
        tag[0] = 'B'; tag[1] = 'O'; tag[2] = 'O'; tag[3] = 'T'; tag[4] = 0;
        for (const char *p = "stage "; *p; ) pre[j++] = *p++;
        for (unsigned k = 6; k < n && j < 8; k++) pre[j++] = t[k];
        pre[j++] = ':'; pre[j++] = ' '; pre[j] = 0;
        return rest;
    }
    unsigned j = 0;
    for (; j < n && j < TAG_COLS - 1; j++) tag[j] = t[j];
    tag[j] = 0;
    return rest;
}

static uint16_t line_colour(const char *l)
{
    if (has(l, "again") || has(l, "retry")) return AL_WAIT;              /* tried again: not a failure yet */
    if (has(l, "FAIL") || has(l, "ERROR") || has(l, "rror") || has(l, "failed")) return AL_FAIL;
    if (has(l, "not mounted") || has(l, "left out") || has(l, "ignored") || has(l, "not available") ||
        has(l, "WARN") || has(l, "no firmware")) return AL_WAIT;
    return AL_TEXT;
}

int al_panel(int which, const char *const *lines, unsigned n)
{
    uint32_t x, w;
    pan_geom(which, &x, &w);
    if (!fb || !w || pan_h < 72) return 0;
    al_fill(x, pan_y, w, pan_h, AL_PANEL);
    al_fill(x, pan_y, w, 26, AL_TITLE);
    al_frame(x, pan_y, w, pan_h, AL_RULE);
    al_fill(x, pan_y + 26, w, 1, AL_RULE);
    al_text(x + 12, pan_y + 5, which ? "USB" : "SYSTEM", AL_BRIGHT, 8);
    al_text(x + 12 + (which ? 4 : 7) * 8, pan_y + 5, which ? "devices" : "log", AL_DIM, 16);
    uint32_t rows = (pan_h - 40) / 16, cols = (w - 24) / 8;
    unsigned first = n > rows ? n - rows : 0;
    for (unsigned i = first, r = 0; i < n; i++, r++) {
        char tag[TAG_COLS + 2], pre[16];
        const char *msg = split_tag(lines[i], tag, pre);
        uint32_t y = pan_y + 34 + r * 16, tx = x + 12, c = cols;
        uint16_t col = line_colour(lines[i]);
        if (!which) {                                    /* SYSTEM: the tag in a column of its own */
            al_text(tx, y, tag, AL_DIM, TAG_COLS);
            tx += TAG_COLS * 8; c = cols > TAG_COLS ? cols - TAG_COLS : 0;
        }
        if (pre[0]) { uint32_t nx = al_text(tx, y, pre, col, c); c -= (nx - tx) / 8; tx = nx; }
        al_text(tx, y, msg, col, c);
    }
    return 1;
}
