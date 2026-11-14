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

/* ---- the options: alice_opts.c, and the System page ---------------------------- */
static const char *const sec_names[] = { "Display", "Storage", "Network", "USB", "System" };
#define NSEC 5
#define SEC_SYSTEM 4
#define items opt_items
#define NITEMS opt_count

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void scpy(char *d, const char *s, unsigned max) { unsigned i = 0; for (; s && s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }

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

void as_init(struct as_state *s, const char *b, const struct al_info *info, unsigned ninfo)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
    opt_init(&s->v, b);
    for (unsigned i = 0; i < ninfo && i < AS_INFO; i++) s->info[i] = info[i];
    s->ninfo = ninfo < AS_INFO ? ninfo : AS_INFO;
}
int as_changed(const struct as_state *s) { return opt_changed(&s->v); }
int as_edits(const struct as_state *s, char *out, unsigned max) { return opt_edits(&s->v, out, max); }

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
    const struct opt_item *it = idx >= 0 ? &items[idx] : 0;
    if (s->editing && it) {
        unsigned n = slen(s->edit);
        if (u == U_ENTER || u == U_KPENT) { scpy(s->v.text[idx], s->edit, AS_TEXT); s->editing = 0; return AS_REDRAW; }
        if (u == U_ESC) { s->editing = 0; return AS_REDRAW; }
        if (u == U_BKSP) { if (n) s->edit[n - 1] = 0; return AS_REDRAW; }
        char c = ascii(u, mods);
        if (c && n < it->maxlen && n + 1 < AS_TEXT) {
            if (it->flags & OT_UPPER && c >= 'a' && c <= 'z') c = (char)(c - 32);
            if ((it->flags & OT_QUOTE) && c == '"') return AS_NONE;     /* it would end the option */
            if ((it->flags & OT_UNDERSCORE) && c == '_') c = ' ';
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
        if (it->kind == OPT_CHOICE) {
            int n = opt_nchoices(it);
            if (u == U_LEFT) s->v.cur[idx] = (uint8_t)((s->v.cur[idx] + n - 1) % n);
            else if (u == U_DEL || u == U_BKSP) s->v.cur[idx] = it->def;
            else s->v.cur[idx] = (uint8_t)((s->v.cur[idx] + 1) % n);
            return AS_REDRAW;
        }
        if (u == U_DEL || u == U_BKSP) { s->v.text[idx][0] = 0; return AS_REDRAW; }
        if (u == U_ENTER || u == U_KPENT || u == U_SPACE) { scpy(s->edit, s->v.text[idx], AS_TEXT); s->editing = 1; return AS_REDRAW; }
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
    const struct opt_item *it = &items[i];
    if (it->kind == OPT_CHOICE) { scpy(out, it->names[s->v.cur[i]], max); return; }
    const char *v = s->v.text[i];
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
            const struct opt_item *it = &items[i];
            uint32_t y = ly + 16 + (uint32_t)r * 28;
            if (y + 22 > ly + body_h) break;
            int sel = r == s->row;
            int changed = it->kind == OPT_CHOICE ? s->v.cur[i] != s->v.orig[i] : !!opt_item_changed(&s->v, i);
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
                uint16_t vc = changed ? AL_ACCENT : (it->kind == OPT_TEXTV && !s->v.text[i][0]) ? AL_DIM : AL_TEXT;
                if (sel && it->kind == OPT_CHOICE) {
                    al_text(vx - 16, y, "<", AL_DIM, 1);
                    uint32_t ex = al_text(vx, y, v, vc, vcols);
                    al_text(ex + 8, y, ">", AL_DIM, 1);
                } else al_text(vx, y, v, vc, vcols);
            }
            if (sel) {
                help = it->help;
                if (it->kind == OPT_CHOICE) opt = it->opts[s->v.cur[i]][0] ? it->opts[s->v.cur[i]] : "(nothing: the default)";
                else if (s->v.text[i][0]) {
                    unsigned k = 0;
                    for (const char *p = it->key; *p && k < sizeof optbuf - 2; ) optbuf[k++] = *p++;
                    optbuf[k++] = '=';
                    for (const char *p = it->secret ? "..." : s->v.text[i]; *p && k < sizeof optbuf - 1; p++)
                        optbuf[k++] = (it->flags & OT_UNDERSCORE) && *p == ' ' ? '_' : *p;
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
