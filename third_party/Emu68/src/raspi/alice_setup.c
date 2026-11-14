/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 setup screen - see alice_setup.h.
 *
 *   +----------------------------------------------------------------------+
 *   |  [emblem]                                      Version  0.5.0        |
 *   |----------------------------------------------------------------------|
 *   |  Display   Storage   Network   USB   System                          |
 *   |----------------------------------------------------------------------|
 *   |  Picture                 Sharp          |  Picture                    |
 *   |  Scaling               < Integer >      |  How AmigaOS's picture ...  |
 *   |  Boot log panels         Shown          |                             |
 *   |                                         |  cmdline.txt: aga.scale=1   |
 *   |----------------------------------------------------------------------|
 *   |  [Up][Down] Select  [Left][Right] Change ...      [F10] Save  [Esc]   |
 *   +----------------------------------------------------------------------+
 */
#include "alice_setup.h"

#define MARGIN 24u

/* ---- the options ------------------------------------------------------------- */
enum { K_CHOICE, K_TEXT };
enum { T_PLAIN = 0, T_QUOTE = 1, T_UNDERSCORE = 2, T_UPPER = 4 };

struct item {
    uint8_t sec, kind, def, flags, maxlen, secret;
    const char *label, *help;
    const char *keys;                   /* choices: the options it owns (space-separated names) */
    const char *const *names;           /* choices: as shown */
    const char *const *opts;            /* choices: the option each writes ("" none) */
    const char *key;                    /* text: the option's name */
};

static const char *const sec_names[] = { "Display", "Storage", "Network", "USB", "System" };
#define NSEC 5
#define SEC_SYSTEM 4

static const char *const n_look[] = { "Sharp", "Smooth", "TV", 0 };
static const char *const o_look[] = { "aga.look=0", "aga.look=1", "aga.look=2" };
static const char *const n_scale[] = { "4:3", "Integer", "Fill the screen", "5:4", "16:9", "16:10", "21:9", "32:9", 0 };
static const char *const o_scale[] = { "aga.scale=0", "aga.scale=1", "aga.scale=2", "aga.scale=3", "aga.scale=4",
                                       "aga.scale=5", "aga.scale=6", "aga.scale=7" };
static const char *const n_shown[] = { "Shown", "Hidden", 0 };
static const char *const o_bootlog[] = { "", "aga.nobootlog" };
static const char *const n_hdf[] = { "Mounted", "Mounted read-only", "Off", 0 };
static const char *const o_hdf[] = { "", "hdf.readonly", "hdf=off" };
static const char *const n_onoff[] = { "On", "Off", 0 };
static const char *const n_mounted[] = { "Mounted", "Off", 0 };
static const char *const o_iso[] = { "", "iso=off" };
static const char *const n_exfat[] = { "As config.txt says", "Off", "Read-only", "Read-write", "Read-write, boot from it", 0 };
static const char *const o_exfat[] = { "", "alice68.exfat=off", "alice68.exfat=ro", "alice68.exfat=rw", "alice68.exfat=boot" };
static const char *const o_wifi[] = { "", "wifi=off" };
static const char *const n_eth[] = { "When a program opens it", "On at boot", "Off", 0 };
static const char *const o_eth[] = { "", "eth=on", "eth=off" };
static const char *const n_mac[] = { "Commodore (00:80:10)", "The Pi's own", 0 };
static const char *const o_mac[] = { "", "net.oui=off" };
static const char *const n_usb[] = { "Alice68", "AmigaOS (xhci.device)", 0 };
static const char *const o_usb[] = { "", "frf_usb=neptune" };
static const char *const o_modesw[] = { "", "frf_usb.nomodeswitch" };

static const struct item items[] = {
    { 0, K_CHOICE, 0, 0, 0, 0, "Picture",
      "How AmigaOS's picture is drawn: Sharp keeps every pixel crisp, Smooth filters it, TV adds scanlines and a little blur.",
      "aga.look", n_look, o_look, 0 },
    { 0, K_CHOICE, 1, 0, 0, 0, "Scaling",
      "How the picture fills the screen. Integer scales by whole numbers, so pixels stay square and even; the others stretch to an aspect ratio.",
      "aga.scale", n_scale, o_scale, 0 },
    { 0, K_CHOICE, 0, 0, 0, 0, "Boot log panels",
      "The SYSTEM and USB panels on this screen while Alice68 starts. Hidden leaves the header and the status bar.",
      "aga.nobootlog", n_shown, o_bootlog, 0 },
    { 1, K_CHOICE, 0, 0, 0, 0, "Hardfiles",
      "The .hdf files in the root and the HDF folder of the SD card's FAT or exFAT partition. Every one is mounted; read-only keeps AmigaOS from writing to them.",
      "hdf hdf.readonly hdf.ro", n_hdf, o_hdf, 0 },
    { 1, K_TEXT, 0, T_UNDERSCORE, 60, 0, "Boot hardfile",
      "The hardfile AmigaOS boots from, by file name (.hdf may be left out). Empty: no hardfile boots, and the SD card's own Amiga partition does.",
      0, 0, 0, "hdf.boot" },
    { 1, K_CHOICE, 0, 0, 0, 0, "CD images",
      "The .iso files in the root and the ISO folder, as read-only CD drives CD0:, CD1:, ...",
      "iso", n_mounted, o_iso, 0 },
    { 1, K_CHOICE, 0, 0, 0, 0, "exFAT partition",
      "The card's exFAT partition as an Amiga volume. Read-write lets AmigaOS change it; a volume left half-written is checked and repaired at the next boot. Boot from it makes it the boot drive.",
      "alice68.exfat", n_exfat, o_exfat, 0 },
    { 2, K_CHOICE, 0, 0, 0, 0, "WiFi",
      "The Pi's WiFi chip, for alice68net.device unit 0. Off leaves it powered down.",
      "wifi", n_onoff, o_wifi, 0 },
    { 2, K_TEXT, 0, T_QUOTE, 32, 0, "Network name",
      "The WiFi network joined at boot (its SSID). Empty: join from AmigaOS with Alice68-WiFi or Alice68-Network instead.",
      0, 0, 0, "wifi.ssid" },
    { 2, K_TEXT, 0, T_QUOTE, 63, 1, "Password",
      "The WiFi network's password (WPA2 or WPA3). Stored in cmdline.txt as it is typed.",
      0, 0, 0, "wifi.psk" },
    { 2, K_TEXT, 0, T_UPPER, 2, 0, "Country",
      "Two letters, such as GB, DE or US: the WiFi channels allowed where you are.",
      0, 0, 0, "wifi.country" },
    { 2, K_CHOICE, 0, 0, 0, 0, "Ethernet",
      "The Pi's Ethernet port, for alice68net.device unit 1. Off leaves it to rondoval's genet.device.",
      "eth", n_eth, o_eth, 0 },
    { 2, K_CHOICE, 0, 0, 0, 0, "MAC addresses",
      "Both network units normally use Commodore's OUI, 00:80:10, with the rest of the Pi's own address. The Pi's own keeps its addresses as they are.",
      "net.oui", n_mac, o_mac, 0 },
    { 3, K_CHOICE, 0, 0, 0, 0, "USB-A ports",
      "Alice68 drives the four USB-A ports itself: keyboard, mouse and Neptune's devices. AmigaOS leaves them to rondoval's xhci.device; keyboard and mouse then come from the USB-C port.",
      "frf_usb", n_usb, o_usb, 0 },
    { 3, K_CHOICE, 0, 0, 0, 0, "Dongle mode switch",
      "Realtek USB network adapters start as a driver CD. Alice68 switches them to network mode; Off leaves them as they come.",
      "frf_usb.nomodeswitch", n_onoff, o_modesw, 0 },
};
#define NITEMS ((int)(sizeof items / sizeof *items))

/* ---- small helpers (no C library in the kernel) ------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void scpy(char *d, const char *s, unsigned max) { unsigned i = 0; for (; s && s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* the next option of a command line: its start and length (quotes keep spaces in) */
static const char *next_tok(const char *c, unsigned *len)
{
    while (*c && is_sp(*c)) c++;
    const char *t = c;
    int q = 0;
    while (*c && (q || !is_sp(*c))) { if (*c == '"') q = !q; c++; }
    *len = (unsigned)(c - t);
    return *len ? t : 0;
}
static int tok_is(const char *t, unsigned n, const char *w) { unsigned k = slen(w); if (k != n) return 0; for (unsigned i = 0; i < n; i++) if (t[i] != w[i]) return 0; return 1; }
/* "key=..." with this key */
static int tok_key(const char *t, unsigned n, const char *key) { unsigned k = slen(key); if (n <= k || t[k] != '=') return 0; for (unsigned i = 0; i < k; i++) if (t[i] != key[i]) return 0; return 1; }

static int item_index(int sec, int row)
{
    for (int i = 0, r = 0; i < NITEMS; i++) if (items[i].sec == sec) { if (r == row) return i; r++; }
    return -1;
}
static int rows_in(const struct as_state *s, int sec)
{
    if (sec == SEC_SYSTEM) return (int)s->ninfo;
    int r = 0;
    for (int i = 0; i < NITEMS; i++) if (items[i].sec == sec) r++;
    return r;
}
static int nchoices(const struct item *it) { int n = 0; while (it->names[n]) n++; return n; }

void as_init(struct as_state *s, const char *b, const struct al_info *info, unsigned ninfo)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
    for (int i = 0; i < NITEMS; i++) {
        const struct item *it = &items[i];
        s->cur[i] = it->def;
        unsigned n;
        for (const char *c = b ? b : "", *t; (t = next_tok(c, &n)); c = t + n) {
            if (it->kind == K_CHOICE) {
                for (int k = 0; k < nchoices(it); k++) if (it->opts[k][0] && tok_is(t, n, it->opts[k])) s->cur[i] = (uint8_t)k;
            } else if (tok_key(t, n, it->key)) {
                const char *v = t + slen(it->key) + 1;
                unsigned vn = n - slen(it->key) - 1, k = 0;
                if (vn >= 2 && v[0] == '"' && v[vn - 1] == '"') { v++; vn -= 2; }
                for (; k < vn && k + 1 < AS_TEXT; k++) s->text[i][k] = (it->flags & T_UNDERSCORE) && v[k] == '_' ? ' ' : v[k];
                s->text[i][k] = 0;
            }
        }
        s->orig[i] = s->cur[i];
        scpy(s->otext[i], s->text[i], AS_TEXT);
    }
    for (unsigned i = 0; i < ninfo && i < AS_INFO; i++) s->info[i] = info[i];
    s->ninfo = ninfo < AS_INFO ? ninfo : AS_INFO;
}

int as_changed(const struct as_state *s)
{
    for (int i = 0; i < NITEMS; i++)
        if (items[i].kind == K_CHOICE ? s->cur[i] != s->orig[i] : !seq(s->text[i], s->otext[i])) return 1;
    return 0;
}

int as_edits(const struct as_state *s, char *out, unsigned max)
{
    unsigned n = 0;
#define EPUT(ch) do { if (n + 1 >= max) return -1; out[n++] = (char)(ch); } while (0)
#define ESTR(str) do { for (const char *p_ = (str); *p_; p_++) EPUT(*p_); } while (0)
    for (int i = 0; i < NITEMS; i++) {
        const struct item *it = &items[i];
        if (it->kind == K_CHOICE) {
            if (s->cur[i] == s->orig[i]) continue;
            for (const char *k = it->keys; *k; ) {                 /* -name for each option it owns */
                EPUT('-');
                while (*k && *k != ' ') { EPUT(*k); k++; }
                EPUT('\n');
                while (*k == ' ') k++;
            }
            if (it->opts[s->cur[i]][0]) { EPUT('+'); ESTR(it->opts[s->cur[i]]); EPUT('\n'); }
        } else {
            if (seq(s->text[i], s->otext[i])) continue;
            EPUT('-'); ESTR(it->key); EPUT('\n');
            if (s->text[i][0]) {
                const char *v = s->text[i];
                int sp = 0;
                for (const char *p = v; *p; p++) if (*p == ' ') sp = 1;
                EPUT('+'); ESTR(it->key); EPUT('=');
                int q = (it->flags & T_QUOTE) && sp;
                if (q) EPUT('"');
                for (const char *p = v; *p; p++) EPUT((it->flags & T_UNDERSCORE) && *p == ' ' ? '_' : *p);
                if (q) EPUT('"');
                EPUT('\n');
            }
        }
    }
    out[n] = 0;
    return (int)n;
#undef EPUT
#undef ESTR
}

/* ---- keys ------------------------------------------------------------------- */
#define U_ENTER 0x28
#define U_ESC   0x29
#define U_BKSP  0x2a
#define U_TAB   0x2b
#define U_SPACE 0x2c
#define U_F10   0x43
#define U_HOME  0x4a
#define U_PGUP  0x4b
#define U_DEL   0x4c
#define U_END   0x4d
#define U_PGDN  0x4e
#define U_RIGHT 0x4f
#define U_LEFT  0x50
#define U_DOWN  0x51
#define U_UP    0x52
#define U_KPENT 0x58

/* a US keyboard's character for a usage, 0 none */
static char ascii(uint8_t u, uint8_t mods)
{
    int sh = (mods & 0x22) != 0;
    if (u >= 0x04 && u <= 0x1d) return (char)((sh ? 'A' : 'a') + (u - 0x04));
    if (u >= 0x1e && u <= 0x27) {
        static const char d[] = "1234567890", sd[] = "!@#$%^&*()";
        return sh ? sd[u - 0x1e] : d[u - 0x1e];
    }
    static const struct { uint8_t u; char c, s; } p[] = {
        { 0x2c, ' ', ' ' }, { 0x2d, '-', '_' }, { 0x2e, '=', '+' }, { 0x2f, '[', '{' }, { 0x30, ']', '}' },
        { 0x31, '\\', '|' }, { 0x33, ';', ':' }, { 0x34, '\'', '"' }, { 0x35, '`', '~' }, { 0x36, ',', '<' },
        { 0x37, '.', '>' }, { 0x38, '/', '?' },
    };
    for (unsigned i = 0; i < sizeof p / sizeof *p; i++) if (p[i].u == u) return sh ? p[i].s : p[i].c;
    return 0;
}

int as_key(struct as_state *s, uint8_t u, uint8_t mods)
{
    if (s->dialog) {
        char c = ascii(u, 0);
        int yes = c == 'y' || u == U_ENTER || u == U_KPENT, no = c == 'n' || u == U_ESC;
        if (yes) { int d = s->dialog; s->dialog = 0; return d == 1 ? AS_SAVE : AS_EXIT; }
        if (no) { s->dialog = 0; return AS_REDRAW; }
        return AS_NONE;
    }
    int idx = item_index(s->sec, s->row);
    const struct item *it = idx >= 0 ? &items[idx] : 0;
    if (s->editing && it) {
        unsigned n = slen(s->edit);
        if (u == U_ENTER || u == U_KPENT) { scpy(s->text[idx], s->edit, AS_TEXT); s->editing = 0; return AS_REDRAW; }
        if (u == U_ESC) { s->editing = 0; return AS_REDRAW; }
        if (u == U_BKSP) { if (n) s->edit[n - 1] = 0; return AS_REDRAW; }
        char c = ascii(u, mods);
        if (c && n < it->maxlen && n + 1 < AS_TEXT) {
            if (it->flags & T_UPPER && c >= 'a' && c <= 'z') c = (char)(c - 32);
            if ((it->flags & T_QUOTE) && c == '"') return AS_NONE;     /* it would end the option */
            if ((it->flags & T_UNDERSCORE) && c == '_') c = ' ';
            s->edit[n] = c; s->edit[n + 1] = 0;
            return AS_REDRAW;
        }
        return AS_NONE;
    }
    int rows = rows_in(s, s->sec);
    switch (u) {
    case U_UP:   if (s->row > 0) s->row--; else s->row = rows ? rows - 1 : 0; return AS_REDRAW;
    case U_DOWN: if (s->row + 1 < rows) s->row++; else s->row = 0; return AS_REDRAW;
    case U_HOME: case U_PGUP: s->row = 0; return AS_REDRAW;
    case U_END: case U_PGDN: s->row = rows ? rows - 1 : 0; return AS_REDRAW;
    case U_TAB:
        s->sec = (mods & 0x22) ? (s->sec + NSEC - 1) % NSEC : (s->sec + 1) % NSEC;
        s->row = 0;
        return AS_REDRAW;
    case U_F10: if (!as_changed(s)) return AS_EXIT; s->dialog = 1; return AS_REDRAW;
    case U_ESC: if (!as_changed(s)) return AS_EXIT; s->dialog = 2; return AS_REDRAW;
    case U_LEFT: case U_RIGHT: case U_SPACE: case U_ENTER: case U_KPENT: case U_DEL: case U_BKSP:
        if (!it) {                                          /* System: left and right change section */
            if (u == U_LEFT) { s->sec = (s->sec + NSEC - 1) % NSEC; s->row = 0; return AS_REDRAW; }
            if (u == U_RIGHT) { s->sec = (s->sec + 1) % NSEC; s->row = 0; return AS_REDRAW; }
            return AS_NONE;
        }
        if (it->kind == K_CHOICE) {
            int n = nchoices(it);
            if (u == U_LEFT) s->cur[idx] = (uint8_t)((s->cur[idx] + n - 1) % n);
            else if (u == U_DEL || u == U_BKSP) s->cur[idx] = it->def;
            else s->cur[idx] = (uint8_t)((s->cur[idx] + 1) % n);
            return AS_REDRAW;
        }
        if (u == U_DEL || u == U_BKSP) { s->text[idx][0] = 0; return AS_REDRAW; }
        if (u == U_ENTER || u == U_KPENT || u == U_SPACE) { scpy(s->edit, s->text[idx], AS_TEXT); s->editing = 1; return AS_REDRAW; }
        return AS_NONE;
    default:
        return AS_NONE;
    }
}

/* ---- drawing ---------------------------------------------------------------- */
static uint32_t hdr_h, tab_y, body_y, body_h, foot_y, list_w;

static void geometry(void)
{
    uint32_t h = al_height();
    hdr_h = (al_width() >= 600 && h >= 480) ? 128 : 56;
    tab_y = hdr_h;
    body_y = tab_y + 44 + 16;
    foot_y = h > 40 ? h - 40 : 0;
    body_h = foot_y > body_y + 100 ? foot_y - 16 - body_y : 0;
    uint32_t inner = al_width() - 2 * MARGIN;
    list_w = al_width() >= 1100 ? inner * 3 / 5 : inner;
}

/* text wrapped to cols columns from y; returns the y after it */
static uint32_t wrap(uint32_t x, uint32_t y, const char *t, uint16_t c, uint32_t cols, uint32_t ymax)
{
    while (*t && y + 16 <= ymax) {
        unsigned n = slen(t), take = n;
        if (n > cols) {
            take = cols;
            while (take && t[take] != ' ') take--;
            if (!take) take = cols;
        }
        al_text(x, y, t, c, take);
        t += take;
        while (*t == ' ') t++;
        y += 18;
    }
    return y;
}

static void value_of(const struct as_state *s, int i, char *out, unsigned max)
{
    const struct item *it = &items[i];
    if (it->kind == K_CHOICE) { scpy(out, it->names[s->cur[i]], max); return; }
    const char *v = s->text[i];
    if (!v[0]) { scpy(out, it->key[0] == 'h' ? "None" : "Not set", max); return; }
    if (it->secret) { unsigned n = slen(v), k = 0; for (; k < n && k < 12 && k + 1 < max; k++) out[k] = '*'; out[k] = 0; return; }
    scpy(out, v, max);
}

static void legend(void)
{
    uint32_t y = foot_y + 13, x = MARGIN;
    al_fill(0, foot_y, al_width(), al_height() - foot_y, AL_BG);
    al_fill(0, foot_y, al_width(), 1, AL_RULE);
    x = al_key(x, y, "Up"); x = al_key(x + 4, y, "Down"); x = al_text(x + 8, y, "Select", AL_DIM, 12) + 24;
    x = al_key(x, y, "Left"); x = al_key(x + 4, y, "Right"); x = al_text(x + 8, y, "Change", AL_DIM, 12) + 24;
    x = al_key(x, y, "Enter"); x = al_text(x + 8, y, "Edit", AL_DIM, 8) + 24;
    if (al_width() >= 1200) { x = al_key(x, y, "Tab"); x = al_text(x + 8, y, "Section", AL_DIM, 8) + 24; }
    uint32_t rw = (3 * 8 + 12) + 8 + 16 * 8 + 24 + (3 * 8 + 12) + 8 + 16 * 8;
    if (al_width() > rw + MARGIN && al_width() - MARGIN - rw > x) {
        uint32_t r = al_width() - MARGIN - rw;
        r = al_key(r, y, "F10"); r = al_text(r + 8, y, "Save and restart", AL_TEXT, 16) + 24;
        r = al_key(r, y, "Esc"); al_text(r + 8, y, "Leave, then boot", AL_TEXT, 16);
    }
}

/* the lines wrap() would take */
static uint32_t wrapped_lines(const char *t, uint32_t cols)
{
    uint32_t n = 0;
    while (*t) {
        unsigned len = slen(t), take = len;
        if (len > cols) { take = cols; while (take && t[take] != ' ') take--; if (!take) take = cols; }
        t += take;
        while (*t == ' ') t++;
        n++;
    }
    return n;
}

/* a box in the middle of the screen; with keys, a row of them at its foot ("Y" "Save" "N" "Back") */
static void box(const char *title, const char *text, const char *k1, const char *l1, const char *k2, const char *l2)
{
    uint32_t w = 600;
    if (w > al_width() - 16) w = al_width() - 16;
    uint32_t cols = (w - 32) / 8, lines = wrapped_lines(text, cols);
    uint32_t h = 28 + 16 + lines * 18 + 12 + (k1 ? 36 : 0);
    uint32_t x = (al_width() - w) / 2, y = (al_height() - h) / 2;
    al_fill(x, y, w, h, AL_PANEL);
    al_fill(x, y, w, 28, AL_TITLE);
    al_frame(x, y, w, h, AL_ACCENT);
    al_fill(x, y + 28, w, 1, AL_RULE);
    al_text(x + 16, y + 6, title, AL_BRIGHT, cols);
    wrap(x + 16, y + 44, text, AL_TEXT, cols, y + h);
    if (k1) {
        uint32_t ky = y + h - 30, kx = x + 16;
        kx = al_key(kx, ky, k1); kx = al_text(kx + 8, ky, l1, AL_TEXT, 24) + 24;
        if (k2) { kx = al_key(kx, ky, k2); al_text(kx + 8, ky, l2, AL_TEXT, 24); }
    }
}

void as_message(const char *title, const char *text) { box(title, text, 0, 0, 0, 0); }

void as_draw(const struct as_state *s)
{
    geometry();
    uint32_t W = al_width();
    al_clear();
    struct al_info rows[2] = { { "Version", ALICE_VERSION }, { "Settings", "cmdline.txt" } };
    if (s->ninfo) rows[0] = s->info[0];
    al_header(rows, 2);
    /* the sections */
    al_fill(0, tab_y, W, 44, AL_BG);
    for (int i = 0, x = (int)MARGIN; i < NSEC; i++) {
        uint32_t tw = slen(sec_names[i]) * 8 + 32;
        if (i == s->sec) {
            al_fill((uint32_t)x, tab_y + 8, tw, 28, AL_SEL);
            al_fill((uint32_t)x, tab_y + 34, tw, 2, AL_ACCENT);
        }
        al_text((uint32_t)x + 16, tab_y + 14, sec_names[i], i == s->sec ? AL_BRIGHT : AL_DIM, 16);
        x += (int)tw + 8;
    }
    if (as_changed(s)) al_text(W - MARGIN - 17 * 8, tab_y + 14, "Changes not saved", AL_WAIT, 17);
    al_fill(0, tab_y + 43, W, 1, AL_RULE);
    if (!body_h) { legend(); return; }

    /* the list */
    uint32_t lx = MARGIN, ly = body_y;
    al_fill(lx, ly, list_w, body_h, AL_PANEL);
    al_frame(lx, ly, list_w, body_h, AL_RULE);
    uint32_t lab_w = 22 * 8, vx = lx + 20 + lab_w, vcols = list_w > lab_w + 80 ? (list_w - lab_w - 64) / 8 : 8;
    const char *help = 0, *opt = 0;
    char optbuf[96];
    optbuf[0] = 0;
    if (s->sec == SEC_SYSTEM) {
        for (unsigned r = 0; r < s->ninfo; r++) {
            uint32_t y = ly + 16 + r * 28;
            if ((int)r == s->row) al_fill(lx + 1, y - 6, list_w - 2, 28, AL_SEL);
            al_text(lx + 20, y, s->info[r].label, AL_DIM, 21);
            al_text(vx, y, s->info[r].value, AL_TEXT, vcols);
        }
        help = "Alice68's versions and this Pi. The settings in the other sections are kept in cmdline.txt, "
               "on the SD card's first partition; you can edit that file on a PC or Mac too.";
    } else {
        for (int r = 0; ; r++) {
            int i = item_index(s->sec, r);
            if (i < 0) break;
            const struct item *it = &items[i];
            uint32_t y = ly + 16 + (uint32_t)r * 28;
            if (y + 22 > ly + body_h) break;
            int sel = r == s->row;
            int changed = it->kind == K_CHOICE ? s->cur[i] != s->orig[i] : !seq(s->text[i], s->otext[i]);
            if (sel) { al_fill(lx + 1, y - 6, list_w - 2, 28, AL_SEL); al_fill(lx + 1, y - 6, 3, 28, AL_ACCENT); }
            al_text(lx + 20, y, it->label, sel ? AL_BRIGHT : AL_TEXT, 21);
            char v[AS_TEXT + 4];
            if (sel && s->editing) {
                uint32_t bw = (it->maxlen < vcols ? it->maxlen : vcols) * 8 + 16;
                al_fill(vx - 8, y - 4, bw, 24, AL_BG);
                al_frame(vx - 8, y - 4, bw, 24, AL_ACCENT);
                const char *e = s->edit;
                unsigned n = slen(e), show = vcols > 2 ? vcols - 2 : 1;
                if (n > show) e += n - show;
                uint32_t cx = al_text(vx, y, it->secret ? "" : e, AL_BRIGHT, show);
                if (it->secret) { for (unsigned k = 0; k < n && k < show; k++) cx = al_text(cx, y, "*", AL_BRIGHT, 1); }
                al_fill(cx + 1, y + 1, 7, 14, AL_ACCENT);              /* the cursor */
            } else {
                value_of(s, i, v, sizeof v);
                uint16_t vc = changed ? AL_ACCENT : (it->kind == K_TEXT && !s->text[i][0]) ? AL_DIM : AL_TEXT;
                if (sel && it->kind == K_CHOICE) {
                    al_text(vx - 16, y, "<", AL_DIM, 1);
                    uint32_t ex = al_text(vx, y, v, vc, vcols);
                    al_text(ex + 8, y, ">", AL_DIM, 1);
                } else al_text(vx, y, v, vc, vcols);
            }
            if (sel) {
                help = it->help;
                if (it->kind == K_CHOICE) opt = it->opts[s->cur[i]][0] ? it->opts[s->cur[i]] : "(nothing: the default)";
                else if (s->text[i][0]) {
                    unsigned k = 0;
                    for (const char *p = it->key; *p && k < sizeof optbuf - 2; ) optbuf[k++] = *p++;
                    optbuf[k++] = '=';
                    for (const char *p = it->secret ? "..." : s->text[i]; *p && k < sizeof optbuf - 1; p++)
                        optbuf[k++] = (it->flags & T_UNDERSCORE) && *p == ' ' ? '_' : *p;
                    optbuf[k] = 0;
                    opt = optbuf;
                } else opt = "(nothing: not set)";
            }
        }
    }
    /* the help panel */
    if (list_w < W - 2 * MARGIN) {
        uint32_t hx = MARGIN + list_w + 16, hw = W - MARGIN - hx, cols = (hw - 32) / 8;
        al_fill(hx, ly, hw, body_h, AL_PANEL);
        al_frame(hx, ly, hw, body_h, AL_RULE);
        int i = item_index(s->sec, s->row);
        al_text(hx + 16, ly + 16, s->sec == SEC_SYSTEM ? "About" : i >= 0 ? items[i].label : "", AL_BRIGHT, cols);
        al_fill(hx + 16, ly + 38, hw - 32, 1, AL_RULE);
        uint32_t y = wrap(hx + 16, ly + 52, help ? help : "", AL_TEXT, cols, ly + body_h - 60);
        if (opt) {
            y += 12;
            al_text(hx + 16, y, "In cmdline.txt", AL_DIM, cols);
            al_text(hx + 16, y + 20, opt, AL_TEXT, cols);
        }
    }
    legend();
    if (s->dialog == 1)
        box("Save and restart", "The changes are written to cmdline.txt on the SD card while AmigaOS starts. "
            "The Pi then restarts with them.", "Y", "Save", "N", "Back");
    else if (s->dialog == 2)
        box("Leave without saving", "Your changes are dropped, and Alice68 boots with the settings it has.", "Y", "Leave", "N", "Back");
}
