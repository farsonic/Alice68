/* SPDX-License-Identifier: GPL-2.0-only
 *
 * cardcore: the SD card Alice68 booted from, without a window (cardcore.h). What
 * Alice68-Card (amiga/card.c) did behind its gadgets, for Alice68Prefs and `Alice68 CARD`.
 */
#include "wifi/wifios.h"
#include "cardcore.h"
#include "hdf/cmdedit.h"
#include "../third_party/Emu68/src/alice/alice_net.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

struct sb { char *p; unsigned n, max; };
static void sc(struct sb *b, char c) { if (b->n + 1 < b->max) b->p[b->n++] = c; b->p[b->n] = 0; }
static void ss(struct sb *b, const char *s) { while (s && *s) sc(b, *s++); }
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void scpy(char *d, const char *s, unsigned max) { unsigned i = 0; for (; s && s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ieq_n(const char *a, const char *b, unsigned n) { for (unsigned i = 0; i < n; i++) if (lc(a[i]) != lc(b[i])) return 0; return 1; }
static int ieq(const char *a, const char *b) { unsigned n = slen(a); return n == slen(b) && ieq_n(a, b, n); }
int cc_ends(const char *s, const char *x) { unsigned n = slen(s), k = slen(x); return n >= k && ieq_n(s + n - k, x, k); }

#define BIG 65536u
struct state {
    struct MsgPort *port;
    struct IOStdReq *io;
    int open;
    UBYTE *big;                                     /* lists and files, BIG bytes */
    char cmd_file[1600];                            /* cmdline.txt as on the card */
    char cmd_base[1600];                            /* what the options apply to (the file, or a line typed) */
    char cmd_out[1200];                             /* what cc_save() writes */
    char edits[1024];
    struct opt_vals ov;
    char cfg_file[8192];                            /* config.txt as on the card */
    char now[8192];
    char lines[CC_MAXL][CC_LINE];
    int nlines;
    char roms[CC_MAXROM][64];
    int nroms, rom_cur;
    char bted[1000], bttmp[1600];
};
static struct state *S;

static const char *const errs[] = {
    "done", "not found", "a file or folder of that name is there already", "the partition is full",
    "in use (a mounted hardfile or CD image, an open file, or a folder volume)",
    "read-only (the card, or the exFAT partition without alice68.exfat=rw)", "the card reported an error",
    "not a name the card can hold", "the folder is not empty", "that partition is not available",
    "too large to load", "that is a folder", "not a folder" };
const char *cc_err(int e)
{
    if (e < 0) e = -e;
    if (e < (int)(sizeof errs / sizeof *errs)) return errs[e];
    return "failed";
}

int cc_open(void)
{
    if (S) return 0;
    S = AllocMem(sizeof *S, MEMF_CLEAR);
    if (!S) return -1;
    S->big = AllocMem(BIG + 64, MEMF_CLEAR);
    S->port = CreateMsgPort();
    S->io = S->port ? CreateIORequest(S->port, sizeof(struct IOStdReq)) : 0;
    if (!S->big || !S->io) { cc_close(); return -1; }
    S->rom_cur = -1;
    if (OpenDevice("alice68hdf.device", CS_UNIT, S->io, 0)) return CSE_NOVOL;
    S->open = 1;
    return 0;
}

void cc_close(void)
{
    if (!S) return;
    if (S->open) CloseDevice(S->io);
    if (S->io) DeleteIORequest(S->io);
    if (S->port) DeleteMsgPort(S->port);
    if (S->big) FreeMem(S->big, BIG + 64);
    FreeMem(S, sizeof *S);
    S = 0;
}

static int cs(UWORD cmd, APTR data, ULONG len, ULONG off)
{
    if (!S || !S->open) return CSE_NOVOL;
    S->io->io.io_Command = cmd; S->io->io_Data = data; S->io->io_Length = len; S->io->io_Offset = off;
    return DoIO(S->io);
}
static struct cs_file *file_req(ULONG vol, const char *path, const char *path2)
{
    struct cs_file *f = (struct cs_file *)S->big;
    memset(f, 0, sizeof *f);
    f->vol = vol;
    scpy(f->path, path, CS_PATH);
    if (path2) scpy(f->path2, path2, CS_PATH);
    return f;
}
/* a root text file into buf (NUL-terminated); its length, 0 when it is not there, or -CSE_* */
static LONG read_text(const char *name, char *buf, ULONG max)
{
    struct cs_file *f = file_req(CS_FAT, name, 0);
    f->max = BIG - sizeof *f;
    int e = cs(CS_READ, f, BIG, 0);
    buf[0] = 0;
    if (e) return e == CSE_NOTFOUND ? 0 : -e;
    ULONG n = S->io->io_Actual < max - 1 ? S->io->io_Actual : max - 1;
    memcpy(buf, f->data, n);
    buf[n] = 0;
    return (LONG)n;
}
static int write_text(const char *name, const char *text, ULONG n)
{
    struct cs_file *f = file_req(CS_FAT, name, 0);
    if (n > BIG - sizeof *f) return CSE_TOOBIG;
    f->len = n;
    memcpy(f->data, text, n);
    return cs(CS_WRITE, f, sizeof *f + n, 0);
}

int cc_info(struct cs_info *out, int count_free)
{
    memset(out, 0, sizeof *out);
    out->version = CS_VERSION;
    return cs(CS_INFO, out, sizeof *out, count_free ? 1 : 0);
}

/* ---- files ---------------------------------------------------------------------------- */
static int before(const struct cc_ent *a, const struct cc_ent *b)     /* folders first, then by name */
{
    int da = (a->flags & CSF_DIR) != 0, db = (b->flags & CSF_DIR) != 0;
    if (da != db) return da;
    const char *x = a->name, *y = b->name;
    while (*x && lc(*x) == lc(*y)) { x++; y++; }
    return lc(*x) < lc(*y);
}
int cc_list(unsigned vol, const char *path, struct cc_ent *e, int max, int *n, int *more)
{
    *n = 0; *more = 0;
    if (!S) return CSE_NOVOL;
    struct cs_list *l = (struct cs_list *)S->big;
    memset(l, 0, sizeof *l);
    l->vol = vol;
    scpy(l->path, path, CS_PATH);
    l->max = (BIG - sizeof *l) / sizeof(struct cs_ent);
    if (l->max > (ULONG)max) l->max = (ULONG)max;
    int err = cs(CS_LIST, l, BIG, 0);
    if (err) return err;
    for (ULONG i = 0; i < l->count && (int)i < max; i++) {
        struct cc_ent t;
        scpy(t.name, l->ent[i].name, 108);
        t.size = l->ent[i].size; t.sizehi = l->ent[i].sizehi; t.flags = l->ent[i].flags;
        int j = *n;                                      /* insertion: a few hundred at most */
        while (j > 0 && before(&t, &e[j - 1])) { e[j] = e[j - 1]; j--; }
        e[j] = t;
        (*n)++;
    }
    *more = l->more != 0;
    return 0;
}
int cc_delete(unsigned vol, const char *path) { return S ? cs(CS_DELETE, file_req(vol, path, 0), BIG, 0) : CSE_NOVOL; }
int cc_rename(unsigned vol, const char *a, const char *b) { return S ? cs(CS_RENAME, file_req(vol, a, b), BIG, 0) : CSE_NOVOL; }
int cc_mkdir(unsigned vol, const char *path) { return S ? cs(CS_MKDIR, file_req(vol, path, 0), BIG, 0) : CSE_NOVOL; }
void cc_child(char *out, const char *dir, const char *name)
{
    char t[CS_PATH];
    struct sb b = { t, 0, CS_PATH };
    t[0] = 0;
    ss(&b, dir); if (dir[0]) sc(&b, '/'); ss(&b, name);
    scpy(out, t, CS_PATH);
}
void cc_parent(char *path)
{
    unsigned n = slen(path);
    while (n && path[n - 1] != '/') n--;
    path[n ? n - 1 : 0] = 0;
}
int cc_is_rom(const char *n, unsigned int size)
{
    if (cc_ends(n, ".rom")) return 1;
    if (cc_ends(n, ".hdf") || cc_ends(n, ".iso") || cc_ends(n, ".img") || cc_ends(n, ".txt") || cc_ends(n, ".dtb") ||
        cc_ends(n, ".dat") || cc_ends(n, ".elf")) return 0;
    static const ULONG sizes[] = { 262144, 524288, 1048576, 2097152, 524288 + 901120, 524288 + 1802240, 262144 + 901120, 262144 + 1802240 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) if (size == sizes[i]) return 1;
    return 0;
}

/* ---- cmdline.txt ---------------------------------------------------------------------- */
static void cmd_compute(void)
{
    int n = opt_edits(&S->ov, S->edits, sizeof S->edits);
    if (n < 0 || cmd_apply(S->cmd_base, n > 0 ? S->edits : "", S->cmd_out, sizeof S->cmd_out) < 0)
        scpy(S->cmd_out, S->cmd_base, sizeof S->cmd_out);
}
struct opt_vals *cc_opts(void) { return &S->ov; }
const char *cc_cmdline(void) { cmd_compute(); return S->cmd_out; }

int cc_secret_item(void) { for (int i = 0; i < opt_count; i++) if (opt_items[i].secret) return i; return -1; }
static int at_psk(const char *line, const char *p)
{
    static const char k[] = "wifi.psk=";
    if (p != line && p[-1] != ' ') return 0;
    for (int i = 0; k[i]; i++) if (p[i] != k[i]) return 0;
    return 1;
}
void cc_cmdline_shown(char *out, unsigned max)
{
    const char *in = cc_cmdline();
    struct sb b = { out, 0, max };
    out[0] = 0;
    for (const char *p = in; *p; ) {
        if (*p == '\n' || *p == '\r') { p++; continue; }
        if (at_psk(in, p)) {
            ss(&b, "wifi.psk=" CC_PMASK);
            p += 9;
            int q = *p == '"';
            if (q) p++;
            while (*p && (q ? *p != '"' : *p != ' ' && *p != '\n')) p++;
            if (q && *p == '"') p++;
            continue;
        }
        sc(&b, *p++);
    }
}
void cc_cmdline_typed(const char *in)
{
    char real[96] = "";
    int si = cc_secret_item();
    if (si >= 0) opt_written(&S->ov, si, real, sizeof real);
    struct sb b = { S->cmd_base, 0, sizeof S->cmd_base };
    S->cmd_base[0] = 0;
    for (const char *p = in; *p; ) {
        if (at_psk(in, p)) {
            const char *v = p + 9;
            int q = *v == '"';
            const char *m = v + q;
            int masked = 1;
            for (int i = 0; CC_PMASK[i]; i++) if (m[i] != CC_PMASK[i]) masked = 0;
            if (masked && (q ? m[8] == '"' : !m[8] || m[8] == ' ')) {
                ss(&b, real);
                p = m + 8 + q;
                continue;
            }
        }
        sc(&b, *p == '\n' || *p == '\r' ? ' ' : *p);
        p++;
    }
    opt_init(&S->ov, S->cmd_base);
}
void cc_option_shown(int i, char *out, unsigned max)
{
    const struct opt_item *it = &opt_items[i];
    if (it->kind == OPT_CHOICE) scpy(out, it->names[S->ov.cur[i]], max);
    else scpy(out, it->secret && S->ov.text[i][0] ? CC_PMASK : S->ov.text[i], max);
}
void cc_set_text(int i, const char *v)
{
    const struct opt_item *it = &opt_items[i];
    unsigned k = 0, max = it->maxlen && it->maxlen < OPT_TEXT ? it->maxlen : OPT_TEXT - 1;
    for (; v[k] && k < max; k++) {
        char c = v[k];
        if ((it->flags & OT_UPPER) && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((it->flags & OT_QUOTE) && c == '"') c = '\'';
        S->ov.text[i][k] = c;
    }
    S->ov.text[i][k] = 0;
}
/* KEY=VALUE for a text option; an option as a choice writes it ("aga.scale=4"); LABEL=NAME */
int cc_set_option(const char *w)
{
    unsigned eq = 0;
    while (w[eq] && w[eq] != '=') eq++;
    for (int i = 0; i < opt_count; i++) {
        const struct opt_item *it = &opt_items[i];
        if (it->kind == OPT_TEXTV && w[eq] && slen(it->key) == eq && ieq_n(w, it->key, eq)) {
            cc_set_text(i, w + eq + 1);
            return 0;
        }
    }
    for (int i = 0; i < opt_count; i++) {
        const struct opt_item *it = &opt_items[i];
        if (it->kind != OPT_CHOICE) continue;
        for (int c = 0; it->names[c]; c++)
            if ((it->opts[c][0] && ieq(w, it->opts[c])) ||
                (w[eq] && slen(it->label) == eq && ieq_n(w, it->label, eq) && ieq(w + eq + 1, it->names[c]))) {
                S->ov.cur[i] = (UBYTE)c;
                return 0;
            }
    }
    return -1;
}
void cc_set_boot(const char *name)
{
    for (int i = 0; i < opt_count; i++)
        if (opt_items[i].kind == OPT_TEXTV && seq(opt_items[i].key, "hdf.boot")) {
            scpy(S->ov.text[i], name, OPT_TEXT);
            unsigned n = slen(S->ov.text[i]);
            if (n > 4 && cc_ends(S->ov.text[i], ".hdf")) S->ov.text[i][n - 4] = 0;    /* "Workbench.hdf": hdf.boot=Workbench */
        }
}

/* ---- config.txt, line by line, and the Kickstart (initramfs NAME followkernel) --------- */
static void cfg_split(const char *t)
{
    S->nlines = 0;
    while (*t && S->nlines < CC_MAXL) {
        unsigned k = 0;
        while (*t && *t != '\n') { if (*t != '\r' && k < CC_LINE - 1) S->lines[S->nlines][k++] = *t; t++; }
        S->lines[S->nlines++][k] = 0;
        if (*t) t++;
    }
}
static ULONG cfg_join(char *out, ULONG max)
{
    struct sb b = { out, 0, (unsigned)max };
    out[0] = 0;
    for (int i = 0; i < S->nlines; i++) { ss(&b, S->lines[i]); sc(&b, '\n'); }
    return b.n;
}
int cc_nlines(void) { return S->nlines; }
const char *cc_line(int i) { return i >= 0 && i < S->nlines ? S->lines[i] : ""; }
static void roms_scan(void);
int cc_line_set(int i, const char *s)
{
    if (i < 0 || i > S->nlines || (i == S->nlines && S->nlines >= CC_MAXL)) return -1;
    if (i == S->nlines) S->nlines++;
    scpy(S->lines[i], s, CC_LINE);
    roms_scan();
    return 0;
}
int cc_line_insert(int i, const char *s)
{
    if (i < 0 || i > S->nlines || S->nlines >= CC_MAXL) return -1;
    for (int k = S->nlines; k > i; k--) scpy(S->lines[k], S->lines[k - 1], CC_LINE);
    S->nlines++;
    scpy(S->lines[i], s, CC_LINE);
    roms_scan();
    return 0;
}
int cc_line_delete(int i)
{
    if (i < 0 || i >= S->nlines) return -1;
    for (int k = i; k + 1 < S->nlines; k++) scpy(S->lines[k], S->lines[k + 1], CC_LINE);
    S->nlines--;
    roms_scan();
    return 0;
}
static int initramfs_line(void)
{
    for (int i = 0; i < S->nlines; i++) {
        const char *l = S->lines[i];
        while (*l == ' ' || *l == '\t') l++;
        if (ieq_n(l, "initramfs", 9) && (l[9] == ' ' || l[9] == '\t')) return i;
    }
    return -1;
}
static const char *after_initramfs(int i)
{
    const char *l = S->lines[i];
    while (*l == ' ' || *l == '\t') l++;
    l += 9;
    while (*l == ' ' || *l == '\t') l++;
    return l;
}
static void roms_scan(void)
{
    struct cs_list *l = (struct cs_list *)S->big;
    char cur[64] = "";
    int il = initramfs_line();
    if (il >= 0) {
        const char *p = after_initramfs(il);
        unsigned k = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && k < 63) cur[k++] = *p++;
        cur[k] = 0;
    }
    memset(l, 0, sizeof *l);
    l->vol = CS_FAT;
    l->max = (BIG - sizeof *l) / sizeof(struct cs_ent);
    S->nroms = 0;
    if (!cs(CS_LIST, l, BIG, 0))
        for (ULONG i = 0; i < l->count && S->nroms < CC_MAXROM - 1; i++)
            if (!(l->ent[i].flags & CSF_DIR) && cc_is_rom(l->ent[i].name, l->ent[i].size)) scpy(S->roms[S->nroms++], l->ent[i].name, 64);
    S->rom_cur = -1;
    for (int i = 0; i < S->nroms; i++) if (cur[0] && ieq(S->roms[i], cur)) S->rom_cur = i;
    if (S->rom_cur < 0 && cur[0] && S->nroms < CC_MAXROM) { scpy(S->roms[S->nroms], cur, 64); S->rom_cur = S->nroms++; }
}
int cc_nroms(void) { return S->nroms; }
const char *cc_rom(int i) { return i >= 0 && i < S->nroms ? S->roms[i] : ""; }
int cc_rom_cur(void) { return S->rom_cur; }
int cc_set_kickstart(const char *name)
{
    char t[CC_LINE];
    struct sb b = { t, 0, CC_LINE };
    int il = initramfs_line();
    t[0] = 0;
    ss(&b, "initramfs "); ss(&b, name);
    if (il < 0) {
        ss(&b, " followkernel");
        return cc_line_set(S->nlines, t);
    }
    const char *l = after_initramfs(il);
    while (*l && *l != ' ' && *l != '\t' && *l != ',') l++;
    ss(&b, *l ? l : " followkernel");
    return cc_line_set(il, t);
}

/* ---- loading and saving --------------------------------------------------------------- */
int cc_load(void)
{
    LONG a = read_text("cmdline.txt", S->cmd_file, sizeof S->cmd_file);
    LONG b = read_text("config.txt", S->cfg_file, sizeof S->cfg_file);
    scpy(S->cmd_base, S->cmd_file, sizeof S->cmd_base);
    for (char *p = S->cmd_base; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    opt_init(&S->ov, S->cmd_base);
    cfg_split(S->cfg_file);
    roms_scan();
    cmd_compute();
    return a < 0 ? (int)-a : b < 0 ? (int)-b : 0;
}
int cc_changed(void)
{
    cmd_compute();
    cfg_join(S->now, sizeof S->now);
    return (!seq(S->cmd_out, S->cmd_file) ? 1 : 0) | (!seq(S->now, S->cfg_file) ? 2 : 0);
}
int cc_save(int *failed)
{
    int ch = cc_changed(), e;
    ULONG cn = slen(S->now);
    *failed = 0;
    if (ch & 1) {
        if (S->cmd_file[0]) write_text("cmdline.txt.bak", S->cmd_file, slen(S->cmd_file));
        if ((e = write_text("cmdline.txt", S->cmd_out, slen(S->cmd_out)))) { *failed = 1; return -e; }
    }
    if (ch & 2) {
        if (S->cfg_file[0]) write_text("config.txt.bak", S->cfg_file, slen(S->cfg_file));
        if ((e = write_text("config.txt", S->now, cn))) { *failed = 2; return -e; }
    }
    if (ch) cc_load();
    return ch;
}

/* ---- the network board ---------------------------------------------------------------- */
volatile unsigned char *cc_board(void)
{
    APTR eb = OpenLibrary("expansion.library", 36);
    if (!eb) return 0;
    struct ConfigDev *cd = FindConfigDev(eb, 0, AN_MANUFACTURER, AN_PRODUCT);
    CloseLibrary(eb);
    if (!cd) return 0;
    volatile struct an_hdr *h = *(volatile struct an_hdr **)((UBYTE *)cd + 32);    /* cd_BoardAddr */
    return h && h->magic == AN_MAGIC && h->version == AN_VERSION ? (volatile unsigned char *)h : 0;
}
int cc_restart(void)
{
    volatile unsigned char *b = cc_board();
    if (!b) return -1;
    ((volatile struct an_hdr *)b)->restart = AN_RESTART;
    return 0;
}

static volatile struct an_bt *btb(void) { volatile unsigned char *b = cc_board(); return b ? (volatile struct an_bt *)(b + AN_OFF_BT) : 0; }
int cc_bt_cmd(unsigned cmd, const volatile unsigned char *addr)
{
    volatile struct an_bt *x = btb();
    if (!x) return -1;
    if (x->state != AN_BT_READY) return -2;
    for (int i = 0; i < 6; i++) x->cmd_addr[i] = addr ? addr[i] : 0;
    x->cmd = cmd;
    x->cmd_seq = x->cmd_seq + 1;
    return 0;
}
static const char hx[] = "0123456789abcdef";
void cc_bt_addr(char *out, const volatile unsigned char *a)
{
    static const char HX[] = "0123456789ABCDEF";
    int k = 0;
    for (int i = 5; i >= 0; i--) { out[k++] = HX[a[i] >> 4]; out[k++] = HX[a[i] & 15]; if (i) out[k++] = ':'; }
    out[k] = 0;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : lc(c) >= 'a' && lc(c) <= 'f' ? lc(c) - 'a' + 10 : -1; }
int cc_bt_parse_addr(const char *s, unsigned char *a)
{
    for (int i = 5; i >= 0; i--) {
        int h = hexv(s[0]), l = h >= 0 ? hexv(s[1]) : -1;
        if (h < 0 || l < 0) return -1;
        a[i] = (unsigned char)(h << 4 | l);
        s += 2;
        if (i && *s == ':') s++;
    }
    return *s && *s != ' ' && *s != '\n' ? -1 : 0;
}
const char *cc_bt_kind(unsigned f)
{
    return f & AN_BTK_PAD ? "game pad" : f & AN_BTK_KEYBOARD ? (f & AN_BTK_MOUSE ? "keyboard+mouse" : "keyboard") :
           f & AN_BTK_MOUSE ? "mouse" : "device";
}
const char *cc_bt_found_kind(unsigned cod)
{
    unsigned m2 = (cod >> 6) & 3, sub = (cod >> 2) & 15;           /* sub: 1 joystick, 2 game pad */
    return sub == 2 ? "game pad" : sub == 1 ? "joystick" : m2 == 3 ? "keyboard+mouse" : m2 == 1 ? "keyboard" : m2 == 2 ? "mouse" : "device";
}
const char *cc_bt_state(unsigned s)
{
    static const char *const st[] = { "off", "waiting for its firmware (BCM4345C0.hcd in the WIFI folder)", "starting", "ready", "FAILED" };
    return s < 5 ? st[s] : "?";
}
/* the paired list as bt.dev= options, into cmdline.txt at once */
int cc_bt_keep(void)
{
    volatile struct an_bt *x = btb();
    if (!x || !S) return CSE_NOVOL;
    struct sb b = { S->bted, 0, sizeof S->bted };
    S->bted[0] = 0;
    ss(&b, "-bt.dev\n");
    for (ULONG i = 0; i < x->nkeys && i < AN_BT_KEYS; i++) {
        volatile struct an_btkey *k = &x->keys[i];
        ss(&b, "+bt.dev=");
        for (int j = 5; j >= 0; j--) { sc(&b, hx[k->addr[j] >> 4]); sc(&b, hx[k->addr[j] & 15]); }
        sc(&b, ':');
        for (int j = 0; j < 16; j++) { sc(&b, hx[k->key[j] >> 4]); sc(&b, hx[k->key[j] & 15]); }
        sc(&b, ':'); sc(&b, hx[((k->flags >> 1) & 3) | (k->flags & AN_BTK_LE ? 4 : 0) | (k->flags & AN_BTK_PAD ? 8 : 0)]);
        if (k->flags & AN_BTK_LE) {                  /* :EDIV:RAND:T[:IRK] */
            volatile struct an_btle *e = &x->le[i];
            sc(&b, ':'); for (int s = 12; s >= 0; s -= 4) sc(&b, hx[(e->ediv >> s) & 15]);
            sc(&b, ':'); for (int j = 0; j < 8; j++) { sc(&b, hx[e->rand[j] >> 4]); sc(&b, hx[e->rand[j] & 15]); }
            sc(&b, ':'); sc(&b, (char)('0' + (k->addr[6] & 1)));
            if (k->addr[7]) { sc(&b, ':'); for (int j = 0; j < 16; j++) { sc(&b, hx[e->irk[j] >> 4]); sc(&b, hx[e->irk[j] & 15]); } }
        }
        sc(&b, '\n');
    }
    if (cmd_apply(S->cmd_file, S->bted, S->bttmp, sizeof S->bttmp) < 0) return -1;
    int e = write_text("cmdline.txt", S->bttmp, slen(S->bttmp));
    if (e) return e;
    scpy(S->cmd_file, S->bttmp, sizeof S->cmd_file);
    if (cmd_apply(S->cmd_base, S->bted, S->bttmp, sizeof S->bttmp) >= 0) {
        scpy(S->cmd_base, S->bttmp, sizeof S->cmd_base);
        for (char *p = S->cmd_base; *p; p++) if (*p == '\n') *p = ' ';
    }
    return 0;
}
