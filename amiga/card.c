/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-Card - the SD card Alice68 booted from, in a GadTools window.
 *
 *   Card        the partitions (FAT, exFAT, Emu68's own), free space, what is
 *               mounted from the card
 *   Files       browse the FAT and exFAT partitions; delete, rename, new
 *               folder; a hardfile to boot from; a Kickstart ROM to start
 *   Settings    cmdline.txt's options (the same table as the setup screen,
 *               src/raspi/alice_opts.c) and the Kickstart ROM (config.txt)
 *   Text files  config.txt line by line, cmdline.txt as it will be saved
 *
 * Everything goes through alice68hdf.device's card service (hdf/cardsvc.h),
 * which writes with the device's own FAT/exFAT engines, so mounted hardfiles,
 * CD images and open files are safe. Save writes cmdline.txt and config.txt
 * (the old ones kept as cmdline.txt.bak and config.txt.bak); Restart asks the
 * kernel to restart the Pi through the network board.
 */
#include "wifi/wifios.h"
#include "hdf/cardsvc.h"
#include "hdf/cmdedit.h"
#include "../third_party/Emu68/src/raspi/alice_opts.h"
#include "../third_party/Emu68/src/alice/alice_net.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

#define VERSTR "Alice68-Card 1.0"
static const char verstring[] __attribute__((used)) = "$VER: " VERSTR " (14.11.2026)";
ULONG lvo_y(APTR base, LONG off, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG a0, ULONG a1, ULONG a2, ULONG a3);
#define LY(b, o, d0, d1, d2, d3, a0, a1, a2, a3) lvo_y((APTR)(b), (o), (ULONG)(d0), (ULONG)(d1), (ULONG)(d2), (ULONG)(d3), (ULONG)(a0), (ULONG)(a1), (ULONG)(a2), (ULONG)(a3))

static APTR dos, intui, gt, gfx;
static ULONG out;
static void put(const char *s) { LONG n = 0; while (s[n]) n++; if (out) Write(dos, out, s, n); }

/* ---- intuition, gadtools, graphics ------------------------------------------------ */
static APTR OpenWindowTagList(const struct TagItem *t)  { return (APTR)LY(intui, -606, 0, 0, 0, 0, 0, t, 0, 0); }
static void CloseWindow(APTR w)                         { LY(intui, -72, 0, 0, 0, 0, w, 0, 0, 0); }
static APTR LockPubScreen(const char *n)                { return (APTR)LY(intui, -510, 0, 0, 0, 0, n, 0, 0, 0); }
static void UnlockPubScreen(APTR s)                     { LY(intui, -516, 0, 0, 0, 0, 0, s, 0, 0); }
static UWORD AddGList(APTR w, APTR g, LONG pos, LONG n) { return (UWORD)LY(intui, -438, (ULONG)pos, (ULONG)n, 0, 0, w, g, 0, 0); }
static UWORD RemoveGList(APTR w, APTR g, LONG n)        { return (UWORD)LY(intui, -444, (ULONG)n, 0, 0, 0, w, g, 0, 0); }
static void RefreshGList(APTR g, APTR w, LONG n)        { LY(intui, -432, (ULONG)n, 0, 0, 0, g, w, 0, 0); }
static LONG EasyRequestArgs(APTR w, APTR es, APTR args) { return (LONG)LY(intui, -588, 0, 0, 0, 0, w, es, 0, args); }
static LONG DoubleClick(ULONG s1, ULONG m1, ULONG s2, ULONG m2) { return (LONG)LY(intui, -102, s1, m1, s2, m2, 0, 0, 0, 0); }
static APTR CreateContext(APTR *gl)                     { return (APTR)LY(gt, -114, 0, 0, 0, 0, gl, 0, 0, 0); }
static APTR CreateGadgetA(ULONG kind, APTR prev, APTR ng, const struct TagItem *t) { return (APTR)LY(gt, -30, kind, 0, 0, 0, prev, ng, t, 0); }
static void FreeGadgets(APTR gl)                        { LY(gt, -36, 0, 0, 0, 0, gl, 0, 0, 0); }
static void GT_SetGadgetAttrsA(APTR g, APTR w, const struct TagItem *t) { LY(gt, -42, 0, 0, 0, 0, g, w, 0, t); }
static APTR GT_GetIMsg(APTR port)                       { return (APTR)LY(gt, -72, 0, 0, 0, 0, port, 0, 0, 0); }
static void GT_ReplyIMsg(APTR m)                        { LY(gt, -78, 0, 0, 0, 0, 0, m, 0, 0); }
static void GT_RefreshWindow(APTR w)                    { LY(gt, -84, 0, 0, 0, 0, w, 0, 0, 0); }
static void GT_BeginRefresh(APTR w)                     { LY(gt, -90, 0, 0, 0, 0, w, 0, 0, 0); }
static void GT_EndRefresh(APTR w, LONG c)               { LY(gt, -96, (ULONG)c, 0, 0, 0, w, 0, 0, 0); }
static void DrawBevelBoxA(APTR rp, LONG l, LONG t, LONG w, LONG h, const struct TagItem *tg) { LY(gt, -120, (ULONG)l, (ULONG)t, (ULONG)w, (ULONG)h, rp, tg, 0, 0); }
static APTR GetVisualInfoA(APTR scr, const struct TagItem *t) { return (APTR)LY(gt, -126, 0, 0, 0, 0, scr, t, 0, 0); }
static void FreeVisualInfo(APTR vi)                     { LY(gt, -132, 0, 0, 0, 0, vi, 0, 0, 0); }
static void SetAPen(APTR rp, ULONG p)                   { LY(gfx, -342, p, 0, 0, 0, 0, rp, 0, 0); }
static void RectFill(APTR rp, LONG a, LONG b, LONG c, LONG d) { LY(gfx, -306, (ULONG)a, (ULONG)b, (ULONG)c, (ULONG)d, 0, rp, 0, 0); }

#define GT_(n)          (TAG_USER + 0x80000 + (n))
#define GTLV_Labels     GT_(6)
#define GTLV_ReadOnly   GT_(7)
#define GTTX_Text       GT_(11)
#define GTCY_Labels     GT_(14)
#define GTCY_Active     GT_(15)
#define GTST_String     GT_(45)
#define GTST_MaxChars   GT_(46)
#define GTBB_Recessed   GT_(51)
#define GT_VisualInfo   GT_(52)
#define GTLV_ShowSelected GT_(53)
#define GTLV_Selected   GT_(54)
#define GTTX_Border     GT_(57)
#define GTLV_MakeVisible GT_(78)
#define GA_Disabled     (TAG_USER + 0x30000 + 14)
#define WA_(n)          (TAG_USER + 99 + (n))
#define BUTTON_KIND     1
#define LISTVIEW_KIND   4
#define CYCLE_KIND      7
#define STRING_KIND     12
#define TEXT_KIND       13
#define PLACETEXT_LEFT  1
#define PLACETEXT_IN    0x10
#define IDCMP_REFRESHWINDOW 0x4u
#define IDCMP_MOUSEBUTTONS  0x8u
#define IDCMP_MOUSEMOVE     0x10u
#define IDCMP_GADGETDOWN    0x20u
#define IDCMP_GADGETUP      0x40u
#define IDCMP_CLOSEWINDOW   0x200u
#define IDCMP_VANILLAKEY    0x200000u

struct TextAttr_ { const char *name; UWORD ysize; UBYTE style, flags; };
struct NewGadget_ { WORD left, top, width, height; const char *text; const struct TextAttr_ *ta; UWORD id; ULONG flags; APTR vi; APTR user; };
ACHECK(sizeof(struct NewGadget_) == 30);
struct EasyStruct_ { ULONG size, flags; const char *title, *text, *gadgets; };
struct Node_ { struct Node_ *succ, *pred; UBYTE type; BYTE pri; char *name; };
struct List_ { struct Node_ *head, *tail, *tailpred; UBYTE type, pad; };

/* ---- text helpers ------------------------------------------------------------------- */
struct sb { char *p; unsigned n, max; };
static void sc(struct sb *b, char c) { if (b->n + 1 < b->max) b->p[b->n++] = c; b->p[b->n] = 0; }
static void ss(struct sb *b, const char *s) { while (s && *s) sc(b, *s++); }
static void sd(struct sb *b, ULONG v) { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) sc(b, t[--n]); }
static void spad(struct sb *b, unsigned col) { while (b->n < col) sc(b, ' '); }
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void scpy(char *d, const char *s, unsigned max) { unsigned i = 0; for (; s && s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ieq_n(const char *a, const char *b, unsigned n) { for (unsigned i = 0; i < n; i++) if (lc(a[i]) != lc(b[i])) return 0; return 1; }
static int ends(const char *s, const char *x) { unsigned n = slen(s), k = slen(x); return n >= k && ieq_n(s + n - k, x, k); }
/* "1.5 GB", "512 KB" */
static void skb(struct sb *b, ULONG kb)
{
    if (kb >= 1024u * 1024u) { ULONG t = kb / (1024u * 1024u / 10u); sd(b, t / 10); sc(b, '.'); sd(b, t % 10); ss(b, " GB"); }
    else if (kb >= 1024u) { ULONG t = kb / (1024u / 10u); sd(b, t / 10); if (kb < 100u * 1024u) { sc(b, '.'); sd(b, t % 10); } ss(b, " MB"); }
    else { sd(b, kb); ss(b, " KB"); }
}
static void sbytes(struct sb *b, ULONG lo, ULONG hi)
{
    if (hi) skb(b, hi << 22 | lo >> 10);
    else if (lo < 10240u) { sd(b, lo); ss(b, " B"); }
    else skb(b, lo / 1024u);
}

/* ---- the card service ------------------------------------------------------------- */
static struct MsgPort *csport;
static struct IOStdReq *csio;
static int csopen;
static UBYTE *big;                                   /* 64 KB: lists, files */
#define BIG 65536u
static struct cs_info info;
static char status[160];

static const char *cs_err(int e)
{
    switch (e) {
    case 0: return "done";
    case CSE_NOTFOUND: return "not found";
    case CSE_EXISTS: return "a file or folder of that name is there already";
    case CSE_FULL: return "the partition is full";
    case CSE_INUSE: return "in use (a mounted hardfile or CD image, an open file, or a folder volume)";
    case CSE_RO: return "read-only (the card, or the exFAT partition without alice68.exfat=rw)";
    case CSE_IO: return "the card reported an error";
    case CSE_BADNAME: return "not a name the card can hold";
    case CSE_NOTEMPTY: return "the folder is not empty";
    case CSE_NOVOL: return "that partition is not available";
    case CSE_TOOBIG: return "too large for Alice68-Card to load";
    case CSE_ISDIR: return "that is a folder";
    case CSE_NOTDIR: return "not a folder";
    case -3: return "alice68hdf.device is too old for Alice68-Card";
    default: return "failed";
    }
}
static int cs(UWORD cmd, APTR data, ULONG len, ULONG off)
{
    if (!csopen) return CSE_NOVOL;
    csio->io.io_Command = cmd; csio->io_Data = data; csio->io_Length = len; csio->io_Offset = off;
    return DoIO(csio);
}
static struct cs_file *file_req(ULONG vol, const char *path, const char *path2)
{
    struct cs_file *f = (struct cs_file *)big;
    memset(f, 0, sizeof *f);
    f->vol = vol;
    scpy(f->path, path, CS_PATH);
    if (path2) scpy(f->path2, path2, CS_PATH);
    return f;
}
/* a root text file into buf (NUL-terminated); its length, or -1 */
static LONG read_text(const char *name, char *buf, ULONG max)
{
    struct cs_file *f = file_req(CS_FAT, name, 0);
    f->max = BIG - sizeof *f;
    int e = cs(CS_READ, f, BIG, 0);
    if (e) return e == CSE_NOTFOUND ? 0 : -1;
    ULONG n = csio->io_Actual < max - 1 ? csio->io_Actual : max - 1;
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

/* ---- what the window shows ---------------------------------------------------------- */
enum {
    G_TAB0 = 1, G_TAB1, G_TAB2, G_TAB3, G_SAVE, G_RESTART, G_STATUS,
    /* Card */
    G_CARD, G_PARTS, G_EXFAT, G_USED, G_FREE,
    /* Files */
    G_VOL, G_FILTER, G_PARENT, G_PATH, G_FILES, G_OPEN, G_BOOT, G_KICK, G_DELETE, G_RENAME, G_MKDIR, G_NAME, G_FINFO,
    /* Settings */
    G_KS, G_REVERT, G_HELP0, G_HELP1, G_HELP2,
    /* Text files */
    G_LINES, G_LINE, G_ADDL, G_DELL, G_CMD, G_TNOTE,
    G_OPT0                                           /* G_OPT0 + i: option i (alice_opts.c) */
};
#define G_N (G_OPT0 + OPT_MAX)
static APTR gad[G_N];
static int gpage[G_N];                               /* the page a gadget is on, -1 always there */
static UBYTE *win;
static APTR vi;
static APTR glist_top, glist_page[4];
static int page = -1;
static int x0, y0;

static int on_window(int g) { return gpage[g] < 0 || gpage[g] == page; }
static void setattr(int g, ULONG tag, ULONG val)
{
    struct TagItem t[] = { { tag, val }, { TAG_DONE, 0 } };
    if (gad[g]) GT_SetGadgetAttrsA(gad[g], win && on_window(g) ? win : 0, t);
}
static void textset(int g, const char *s) { setattr(g, GTTX_Text, (ULONG)s); }
static const char *strget(int g) { UBYTE *gg = gad[g]; return gg ? *(const char **)*(APTR *)(gg + 34) : ""; }
static void strset(int g, const char *s) { setattr(g, GTST_String, (ULONG)s); }
static void say(const char *a, const char *b)
{
    struct sb s = { status, 0, sizeof status };
    ss(&s, a); if (b) ss(&s, b);
    textset(G_STATUS, status);
}

/* a list for a LISTVIEW: nodes over an array of lines */
struct lv { struct List_ l; struct Node_ *n; char (*t)[80]; int count, cap; };
static void lv_begin(struct lv *v, int g) { setattr(g, GTLV_Labels, ~0u); v->count = 0; }
static char *lv_add(struct lv *v)
{
    if (v->count >= v->cap) return 0;
    char *t = v->t[v->count];
    t[0] = 0;
    v->n[v->count].name = t;
    v->count++;
    return t;
}
static void lv_end(struct lv *v, int g)
{
    v->l.head = (struct Node_ *)&v->l.tail; v->l.tail = 0; v->l.tailpred = (struct Node_ *)&v->l.head;
    for (int i = 0; i < v->count; i++) {
        struct Node_ *nd = &v->n[i];
        nd->succ = (struct Node_ *)&v->l.tail; nd->pred = v->l.tailpred;
        v->l.tailpred->succ = nd; v->l.tailpred = nd;
    }
    setattr(g, GTLV_Labels, (ULONG)&v->l);
}
static struct Node_ n_parts[CS_MAXPART], n_used[CS_MAXUSED];
static char t_parts[CS_MAXPART][80], t_used[CS_MAXUSED][80];
static struct lv lv_parts = { { 0, 0, 0, 0, 0 }, n_parts, t_parts, 0, CS_MAXPART };
static struct lv lv_used = { { 0, 0, 0, 0, 0 }, n_used, t_used, 0, CS_MAXUSED };

/* ---- Card ------------------------------------------------------------------------- */
static char cardline[120], exline[120];
static const char *kind_name(ULONG k)
{
    static const char *const n[] = { "empty", "FAT16", "FAT32", "exFAT", "Amiga", "extended", "other" };
    return k < 7 ? n[k] : "?";
}
static void card_show(int count_free)
{
    memset(&info, 0, sizeof info);
    int e = cs(CS_INFO, &info, sizeof info, count_free ? 1 : 0);
    struct sb b = { cardline, 0, sizeof cardline };
    if (e) { ss(&b, "the card service did not answer: "); ss(&b, cs_err(e)); textset(G_CARD, cardline); return; }
    ss(&b, info.card_dev); ss(&b, " unit 0, "); ss(&b, info.card_ro ? "read-only" : "read-write");
    ss(&b, info.cmd64 ? ", the whole card" : ", only its first 4 GB");
    textset(G_CARD, cardline);
    lv_begin(&lv_parts, G_PARTS);
    for (ULONG i = 0; i < info.nparts; i++) {
        struct cs_part *p = &info.part[i];
        char *t = lv_add(&lv_parts);
        struct sb l = { t, 0, 80 };
        sd(&l, i + 1); ss(&l, p->boot ? " *" : "  "); spad(&l, 4);
        static const char hx[] = "0123456789abcdef";
        sc(&l, hx[(p->type >> 4) & 15]); sc(&l, hx[p->type & 15]); spad(&l, 8);
        ss(&l, kind_name(p->kind)); spad(&l, 17);
        ss(&l, p->label); spad(&l, 30);
        skb(&l, p->total_kb); spad(&l, 40);
        if (p->free_kb != ~0u) { skb(&l, p->free_kb); ss(&l, " free"); }
        spad(&l, 55);
        ss(&l, p->vol == CS_FAT ? "Alice68's files" : p->vol == CS_EXFAT ? "exFAT volume" : p->kind == CSP_AMIGA ? "Emu68 RDB disk" : "");
    }
    lv_end(&lv_parts, G_PARTS);
    b = (struct sb){ exline, 0, sizeof exline };
    ss(&b, "exFAT: "); ss(&b, info.exfat_rw ? "written by AmigaOS (alice68.exfat=rw)" : info.exfat_why);
    textset(G_EXFAT, exline);
    lv_begin(&lv_used, G_USED);
    for (ULONG i = 0; i < info.nused; i++) {
        char *t = lv_add(&lv_used);
        struct sb l = { t, 0, 80 };
        ss(&l, info.used[i].kind == 1 ? "hardfile" : "CD image"); spad(&l, 10);
        ss(&l, info.used[i].vol == CS_EXFAT ? "exFAT" : "FAT"); spad(&l, 17);
        ss(&l, info.used[i].name);
    }
    lv_end(&lv_used, G_USED);
}

/* ---- Files ---------------------------------------------------------------------- */
#define MAXF 256
struct fent { char name[108]; ULONG size, sizehi, flags; };
/* the large arrays: allocated at the start rather than static (they would be zeros in the file) */
#define MAXL 160
struct bigmem {
    struct fent fe[MAXF];
    struct Node_ n_files[MAXF];
    char t_files[MAXF][80];
    char lines[MAXL][200];
    struct Node_ n_lines[MAXL];
    char t_lines[MAXL][80];
    char cfg_file[8192];
    char now[8192];
};
static struct bigmem *M;
#define fe (M->fe)
#define lines (M->lines)
#define cfg_file (M->cfg_file)
static int nfe, fsel = -1;
static struct lv lv_files;
static ULONG fvol = CS_FAT;
static int filter;
static char path[CS_PATH], pathline[CS_PATH + 32], finfo[160];
static const char *const vol_names[] = { "FAT partition", "exFAT partition", 0 };
static const char *const filter_names[] = { "All files", "Hardfiles", "CD images", "Kickstart ROMs", "WiFi firmware", 0 };

static int is_rom(const char *n, ULONG size)
{
    if (ends(n, ".rom")) return 1;
    if (ends(n, ".hdf") || ends(n, ".iso") || ends(n, ".img") || ends(n, ".txt") || ends(n, ".dtb") || ends(n, ".dat") || ends(n, ".elf")) return 0;
    static const ULONG sizes[] = { 262144, 524288, 1048576, 2097152, 524288 + 901120, 524288 + 1802240, 262144 + 901120, 262144 + 1802240 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) if (size == sizes[i]) return 1;
    return 0;
}
static int shown(int i)
{
    if (fe[i].flags & CSF_DIR) return 1;
    const char *n = fe[i].name;
    switch (filter) {
    case 1: return ends(n, ".hdf");
    case 2: return ends(n, ".iso");
    case 3: return is_rom(n, fe[i].size);
    case 4: return ends(n, ".clm_blob") || ieq_n(n, "cyfmac", 6) || ieq_n(n, "brcmfmac", 8);
    }
    return 1;
}
static int before(int a, int b)                      /* folders first, then by name */
{
    int da = (fe[a].flags & CSF_DIR) != 0, db = (fe[b].flags & CSF_DIR) != 0;
    if (da != db) return da;
    const char *x = fe[a].name, *y = fe[b].name;
    while (*x && lc(*x) == lc(*y)) { x++; y++; }
    return lc(*x) < lc(*y);
}
static int vis[MAXF], nvis;
static void files_load(void)
{
    struct cs_list *l = (struct cs_list *)big;
    memset(l, 0, sizeof *l);
    l->vol = fvol;
    scpy(l->path, path, CS_PATH);
    l->max = (BIG - sizeof *l) / sizeof(struct cs_ent);
    if (l->max > MAXF) l->max = MAXF;
    int e = cs(CS_LIST, l, BIG, 0);
    nfe = 0;
    if (e) say("Files: ", cs_err(e));
    else {
        for (ULONG i = 0; i < l->count; i++) {
            scpy(fe[nfe].name, l->ent[i].name, 108);
            fe[nfe].size = l->ent[i].size; fe[nfe].sizehi = l->ent[i].sizehi; fe[nfe].flags = l->ent[i].flags;
            nfe++;
        }
        if (l->more) say("Files: this folder has more than ", "256 entries; the first 256 are shown");
    }
}
static void files_show(void)
{
    nvis = 0;
    for (int i = 0; i < nfe; i++) if (shown(i)) vis[nvis++] = i;
    for (int i = 1; i < nvis; i++)                   /* insertion sort: a few hundred at most */
        for (int j = i; j > 0 && before(vis[j], vis[j - 1]); j--) { int t = vis[j]; vis[j] = vis[j - 1]; vis[j - 1] = t; }
    lv_begin(&lv_files, G_FILES);
    for (int k = 0; k < nvis; k++) {
        int i = vis[k];
        char *t = lv_add(&lv_files);
        struct sb l = { t, 0, 80 };
        unsigned nl = slen(fe[i].name);
        if (nl > 38) { for (unsigned c = 0; c < 36; c++) sc(&l, fe[i].name[c]); ss(&l, ".."); }
        else ss(&l, fe[i].name);
        if (fe[i].flags & CSF_DIR) sc(&l, '/');
        spad(&l, 40);
        if (fe[i].flags & CSF_DIR) ss(&l, "folder"); else sbytes(&l, fe[i].size, fe[i].sizehi);
        if (fe[i].flags & CSF_INUSE) { spad(&l, 50); ss(&l, "in use"); }
    }
    lv_end(&lv_files, G_FILES);
    fsel = -1;
    struct sb p = { pathline, 0, sizeof pathline };
    ss(&p, fvol == CS_EXFAT ? "exFAT:" : "FAT:"); ss(&p, path[0] ? path : "(root)");
    textset(G_PATH, pathline);
    finfo[0] = 0;
    textset(G_FINFO, finfo);
}
static void files_refresh(void) { files_load(); files_show(); }
static void child_path(char *out, const char *name)
{
    struct sb b = { out, 0, CS_PATH };
    ss(&b, path); if (path[0]) sc(&b, '/'); ss(&b, name);
}

/* ---- Settings: cmdline.txt and the Kickstart ROM -------------------------------------- */
static char cmd_file[1100];                          /* cmdline.txt as on the card */
static char cmd_base[1100];                          /* what the options apply to (the file, or the raw line typed) */
static char cmd_out[1200];                           /* what Save writes */
static struct opt_vals ov;
static char edits[1024];
static char helpl[3][80];
static int opt_gadget_item[G_N];

static void cmd_compute(void)
{
    int n = opt_edits(&ov, edits, sizeof edits);
    if (n < 0 || cmd_apply(cmd_base, n > 0 ? edits : "", cmd_out, sizeof cmd_out) < 0) scpy(cmd_out, cmd_base, sizeof cmd_out);
}
static void help_show(const char *h)
{
    for (int k = 0; k < 3; k++) {
        unsigned n = slen(h), take = n > 76 ? 76 : n;
        if (n > 76) { while (take && h[take] != ' ') take--; if (!take) take = 76; }
        unsigned i = 0;
        for (; i < take && i < 79; i++) helpl[k][i] = h[i];
        helpl[k][i] = 0;
        h += take;
        while (*h == ' ') h++;
        textset(G_HELP0 + k, helpl[k]);
    }
}
static void opts_show(void)
{
    for (int i = 0; i < opt_count; i++) {
        int g = G_OPT0 + i;
        if (opt_items[i].kind == OPT_CHOICE) setattr(g, GTCY_Active, ov.cur[i]);
        else strset(g, ov.text[i]);
    }
}

/* config.txt, line by line */
static int nlines, lsel = -1;
static struct lv lv_lines;
static void cfg_split(const char *t)
{
    nlines = 0;
    while (*t && nlines < MAXL) {
        unsigned k = 0;
        while (*t && *t != '\n') { if (*t != '\r' && k < 199) lines[nlines][k++] = *t; t++; }
        lines[nlines++][k] = 0;
        if (*t) t++;
    }
}
static ULONG cfg_join(char *out, ULONG max)
{
    struct sb b = { out, 0, (unsigned)max };
    for (int i = 0; i < nlines; i++) { ss(&b, lines[i]); sc(&b, '\n'); }
    return b.n;
}
static void lines_show(void)
{
    lv_begin(&lv_lines, G_LINES);
    for (int i = 0; i < nlines; i++) { char *t = lv_add(&lv_lines); scpy(t, lines[i][0] ? lines[i] : " ", 80); }
    lv_end(&lv_lines, G_LINES);
}

/* the Kickstart: config.txt's "initramfs NAME followkernel" */
#define MAXROM 32
static char roms[MAXROM][64];
static const char *rom_labels[MAXROM + 2];
static int nroms, rom_cur = -1;
static int initramfs_line(void)
{
    for (int i = 0; i < nlines; i++) {
        const char *l = lines[i];
        while (*l == ' ' || *l == '\t') l++;
        if (ieq_n(l, "initramfs", 9) && (l[9] == ' ' || l[9] == '\t')) return i;
    }
    return -1;
}
static void rom_name_of_line(int i, char *out)
{
    const char *l = lines[i];
    while (*l == ' ' || *l == '\t') l++;
    l += 9;
    while (*l == ' ' || *l == '\t') l++;
    unsigned k = 0;
    while (*l && *l != ' ' && *l != '\t' && *l != ',' && k < 63) out[k++] = *l++;
    out[k] = 0;
}
static void roms_scan(void)
{
    struct cs_list *l = (struct cs_list *)big;
    memset(l, 0, sizeof *l);
    l->vol = CS_FAT;
    l->max = (BIG - sizeof *l) / sizeof(struct cs_ent);
    nroms = 0;
    char cur[64] = "";
    int il = initramfs_line();
    if (il >= 0) rom_name_of_line(il, cur);
    if (!cs(CS_LIST, l, BIG, 0))
        for (ULONG i = 0; i < l->count && nroms < MAXROM - 1; i++)
            if (!(l->ent[i].flags & CSF_DIR) && is_rom(l->ent[i].name, l->ent[i].size)) scpy(roms[nroms++], l->ent[i].name, 64);
    rom_cur = -1;
    for (int i = 0; i < nroms; i++) if (cur[0] && ieq_n(roms[i], cur, slen(cur) + 1)) rom_cur = i;
    if (rom_cur < 0 && cur[0] && nroms < MAXROM) { scpy(roms[nroms], cur, 64); rom_cur = nroms++; }
    if (!nroms) { scpy(roms[0], "(no Kickstart ROM on the card)", 64); nroms = 1; rom_cur = 0; }
    for (int i = 0; i < nroms; i++) rom_labels[i] = roms[i];
    rom_labels[nroms] = 0;
}
static void set_kickstart(const char *name)
{
    int il = initramfs_line();
    struct sb b;
    if (il < 0) {
        if (nlines >= MAXL) { say("config.txt has too many lines", 0); return; }
        il = nlines++;
        b = (struct sb){ lines[il], 0, 200 };
        ss(&b, "initramfs "); ss(&b, name); ss(&b, " followkernel");
    } else {
        char rest[200];
        const char *l = lines[il];
        while (*l == ' ' || *l == '\t') l++;
        l += 9;
        while (*l == ' ' || *l == '\t') l++;
        while (*l && *l != ' ' && *l != '\t' && *l != ',') l++;
        scpy(rest, *l ? l : " followkernel", 200);
        b = (struct sb){ lines[il], 0, 200 };
        lines[il][0] = 0;
        ss(&b, "initramfs "); ss(&b, name); ss(&b, rest);
    }
    lines_show();
}

/* ---- loading and saving ------------------------------------------------------------- */
static int changed(void)
{
    char *now = M->now;
    cmd_compute();
    cfg_join(now, 8192);
    return !seq(cmd_out, cmd_file) || !seq(now, cfg_file);
}
static void load_all(void)
{
    if (read_text("cmdline.txt", cmd_file, sizeof cmd_file) < 0) cmd_file[0] = 0;
    if (read_text("config.txt", cfg_file, 8192) < 0) cfg_file[0] = 0;
    scpy(cmd_base, cmd_file, sizeof cmd_base);
    for (char *p = cmd_base; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    opt_init(&ov, cmd_base);
    cfg_split(cfg_file);
    roms_scan();
    cmd_compute();
}
static void text_page_show(void)
{
    cmd_compute();
    char shown_cmd[1100];
    scpy(shown_cmd, cmd_out, sizeof shown_cmd);
    for (char *p = shown_cmd; *p; p++) if (*p == '\n') *p = 0;
    strset(G_CMD, shown_cmd);
    lines_show();
}
static int save_all(void)
{
    char *now = M->now;
    cmd_compute();
    ULONG cn = cfg_join(now, 8192);
    int e = 0, wrote = 0;
    if (!seq(cmd_out, cmd_file)) {
        if (cmd_file[0]) write_text("cmdline.txt.bak", cmd_file, slen(cmd_file));
        e = write_text("cmdline.txt", cmd_out, slen(cmd_out));
        if (e) { say("cmdline.txt not saved: ", cs_err(e)); return 0; }
        wrote |= 1;
    }
    if (!seq(now, cfg_file)) {
        if (cfg_file[0]) write_text("config.txt.bak", cfg_file, slen(cfg_file));
        e = write_text("config.txt", now, cn);
        if (e) { say("config.txt not saved: ", cs_err(e)); return 0; }
        wrote |= 2;
    }
    if (!wrote) { say("Nothing to save: cmdline.txt and config.txt are as on the card", 0); return 1; }
    load_all();
    opts_show();
    setattr(G_KS, GTCY_Labels, (ULONG)rom_labels); setattr(G_KS, GTCY_Active, (ULONG)(rom_cur < 0 ? 0 : rom_cur));
    text_page_show();
    say(wrote == 3 ? "Saved cmdline.txt and config.txt" : wrote == 1 ? "Saved cmdline.txt" : "Saved config.txt",
        " (old copies: .bak). Restart the Pi to use them.");
    return 1;
}
static int ask(const char *title, const char *text, const char *buttons)
{
    struct EasyStruct_ es = { sizeof es, 0, title, text, buttons };
    return (int)EasyRequestArgs(win, &es, 0);
}

/* ---- the window ----------------------------------------------------------------------- */
static void page_frame(void)
{
    APTR rp = *(APTR *)(win + 50);
    struct TagItem t[] = { { GT_VisualInfo, (ULONG)vi }, { GTBB_Recessed, 1 }, { TAG_DONE, 0 } };
    DrawBevelBoxA(rp, x0, y0 + 18, 616, 198, t);
}
static void show_page(int p)
{
    if (p == page) return;
    APTR rp = *(APTR *)(win + 50);
    if (page >= 0 && glist_page[page]) RemoveGList(win, glist_page[page], -1);
    page = p;
    SetAPen(rp, 0);
    RectFill(rp, x0, y0 + 18, x0 + 615, y0 + 215);
    if (glist_page[page]) { AddGList(win, glist_page[page], -1, -1); RefreshGList(glist_page[page], win, -1); }
    GT_RefreshWindow(win);
    page_frame();
}

static const struct TextAttr_ topaz = { "topaz.font", 8, 0, 0 };
static struct NewGadget_ ng;
static struct NewGadget_ *NG(int id, int x, int y, int w, int h, const char *txt, ULONG fl)
{
    ng.left = (WORD)(x0 + x); ng.top = (WORD)(y0 + y); ng.width = (WORD)w; ng.height = (WORD)h;
    ng.text = txt; ng.ta = &topaz; ng.id = (UWORD)id; ng.flags = fl; ng.vi = vi; ng.user = 0;
    return &ng;
}
static APTR mk(APTR prev, int pg, ULONG kind, int id, int x, int y, int w, int h, const char *txt, ULONG fl, const struct TagItem *t)
{
    if (!prev) return 0;
    APTR g = CreateGadgetA(kind, prev, NG(id, x, y, w, h, txt, fl), t);
    if (id < G_N) { gad[id] = g; gpage[id] = pg; }
    return g;
}
static int make_gadgets(void)
{
    struct TagItem txt[] = { { GTTX_Border, 1 }, { TAG_DONE, 0 } };
    struct TagItem ro[] = { { GTLV_ReadOnly, 1 }, { TAG_DONE, 0 } };
    struct TagItem sel[] = { { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };
    static const char *const tabs[] = { "Card", "Files", "Settings", "Text files" };
    APTR g = CreateContext(&glist_top);
    for (int i = 0; i < 4; i++) g = mk(g, -1, BUTTON_KIND, G_TAB0 + i, i * 100, 0, 96, 14, tabs[i], PLACETEXT_IN, 0);
    g = mk(g, -1, BUTTON_KIND, G_SAVE, 420, 0, 92, 14, "Save", PLACETEXT_IN, 0);
    g = mk(g, -1, BUTTON_KIND, G_RESTART, 520, 0, 96, 14, "Restart Pi", PLACETEXT_IN, 0);
    g = mk(g, -1, TEXT_KIND, G_STATUS, 0, 220, 616, 12, 0, 0, txt);
    if (!g) return 0;

    /* Card */
    g = CreateContext(&glist_page[0]);
    g = mk(g, 0, TEXT_KIND, G_CARD, 8, 24, 600, 12, 0, 0, txt);
    g = mk(g, 0, LISTVIEW_KIND, G_PARTS, 8, 40, 600, 60, 0, 0, ro);
    g = mk(g, 0, TEXT_KIND, G_EXFAT, 8, 104, 600, 12, 0, 0, txt);
    g = mk(g, 0, LISTVIEW_KIND, G_USED, 8, 120, 600, 68, 0, 0, ro);
    g = mk(g, 0, BUTTON_KIND, G_FREE, 8, 196, 200, 14, "Count free space", PLACETEXT_IN, 0);
    if (!g) return 0;

    /* Files */
    g = CreateContext(&glist_page[1]);
    struct TagItem cy1[] = { { GTCY_Labels, (ULONG)vol_names }, { TAG_DONE, 0 } };
    struct TagItem cy2[] = { { GTCY_Labels, (ULONG)filter_names }, { TAG_DONE, 0 } };
    g = mk(g, 1, CYCLE_KIND, G_VOL, 8, 22, 160, 14, 0, 0, cy1);
    g = mk(g, 1, CYCLE_KIND, G_FILTER, 176, 22, 160, 14, 0, 0, cy2);
    g = mk(g, 1, BUTTON_KIND, G_PARENT, 344, 22, 96, 14, "Parent", PLACETEXT_IN, 0);
    g = mk(g, 1, TEXT_KIND, G_PATH, 8, 40, 432, 12, 0, 0, txt);
    g = mk(g, 1, LISTVIEW_KIND, G_FILES, 8, 56, 432, 124, 0, 0, sel);
    g = mk(g, 1, BUTTON_KIND, G_OPEN, 448, 22, 160, 14, "Open folder", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_BOOT, 448, 56, 160, 14, "Boot from it", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_KICK, 448, 72, 160, 14, "Use as Kickstart", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_RENAME, 448, 104, 160, 14, "Rename to name", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_MKDIR, 448, 120, 160, 14, "New folder", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_DELETE, 448, 152, 160, 14, "Delete...", PLACETEXT_IN, 0);
    struct TagItem st[] = { { GTST_MaxChars, 107 }, { TAG_DONE, 0 } };
    g = mk(g, 1, STRING_KIND, G_NAME, 56, 182, 384, 14, "Name", PLACETEXT_LEFT, st);
    g = mk(g, 1, TEXT_KIND, G_FINFO, 8, 198, 600, 12, 0, 0, 0);
    if (!g) return 0;

    /* Settings: Display and Storage on the left, Network and USB on the right, Kickstart under them */
    g = CreateContext(&glist_page[2]);
    int row[2] = { 0, 0 };
    for (int i = 0; i < opt_count && i < OPT_MAX; i++) {
        const struct opt_item *it = &opt_items[i];
        int col = it->sec >= 2, y = 22 + row[col]++ * 15, x = col ? 452 : 140;
        int id = G_OPT0 + i;
        opt_gadget_item[id] = i;
        if (it->kind == OPT_CHOICE) {
            struct TagItem ct[] = { { GTCY_Labels, (ULONG)it->names }, { GTCY_Active, ov.cur[i] }, { TAG_DONE, 0 } };
            g = mk(g, 2, CYCLE_KIND, id, x, y, 156, 14, it->label, PLACETEXT_LEFT, ct);
        } else {
            struct TagItem tt[] = { { GTST_String, (ULONG)ov.text[i] }, { GTST_MaxChars, it->maxlen }, { TAG_DONE, 0 } };
            g = mk(g, 2, STRING_KIND, id, x, y, 156, 14, it->label, PLACETEXT_LEFT, tt);
        }
    }
    struct TagItem kt[] = { { GTCY_Labels, (ULONG)rom_labels }, { GTCY_Active, (ULONG)(rom_cur < 0 ? 0 : rom_cur) }, { TAG_DONE, 0 } };
    g = mk(g, 2, CYCLE_KIND, G_KS, 140, 22 + row[0] * 15, 156, 14, "Kickstart ROM", PLACETEXT_LEFT, kt);
    for (int k = 0; k < 3; k++) g = mk(g, 2, TEXT_KIND, G_HELP0 + k, 8, 152 + k * 12, 600, 12, 0, 0, 0);
    g = mk(g, 2, BUTTON_KIND, G_REVERT, 8, 196, 200, 14, "Back to the card's", PLACETEXT_IN, 0);
    if (!g) return 0;

    /* Text files */
    g = CreateContext(&glist_page[3]);
    g = mk(g, 3, LISTVIEW_KIND, G_LINES, 8, 22, 600, 116, 0, 0, sel);
    struct TagItem ls[] = { { GTST_MaxChars, 199 }, { TAG_DONE, 0 } };
    g = mk(g, 3, STRING_KIND, G_LINE, 8, 142, 432, 14, 0, 0, ls);
    g = mk(g, 3, BUTTON_KIND, G_ADDL, 448, 142, 76, 14, "Add", PLACETEXT_IN, 0);
    g = mk(g, 3, BUTTON_KIND, G_DELL, 532, 142, 76, 14, "Delete", PLACETEXT_IN, 0);
    struct TagItem cs_[] = { { GTST_MaxChars, 1000 }, { TAG_DONE, 0 } };
    g = mk(g, 3, STRING_KIND, G_CMD, 104, 164, 504, 14, "cmdline.txt", PLACETEXT_LEFT, cs_);
    g = mk(g, 3, TEXT_KIND, G_TNOTE, 8, 182, 600, 12, 0, 0, 0);
    if (!g) return 0;
    return 1;
}

/* ---- events -------------------------------------------------------------------------- */
static ULONG last_s, last_m;
static int last_code = -1;
static void files_click(UWORD code, ULONG secs, ULONG micros)
{
    if (code >= nvis) return;
    int i = vis[code];
    int dbl = last_code == code && DoubleClick(last_s, last_m, secs, micros);
    last_code = code; last_s = secs; last_m = micros;
    fsel = i;
    strset(G_NAME, fe[i].name);
    struct sb b = { finfo, 0, sizeof finfo };
    ss(&b, fe[i].name);
    if (fe[i].flags & CSF_DIR) ss(&b, ": a folder (double-click to open)");
    else { ss(&b, ": "); sbytes(&b, fe[i].size, fe[i].sizehi); }
    if (fe[i].flags & CSF_INUSE) ss(&b, ", in use: it can't be changed now");
    textset(G_FINFO, finfo);
    if (dbl && (fe[i].flags & CSF_DIR)) {
        char np[CS_PATH];
        child_path(np, fe[i].name);
        scpy(path, np, CS_PATH);
        last_code = -1;
        files_refresh();
    }
}
static void files_action(int id)
{
    char p1[CS_PATH], p2[CS_PATH], msg[200];
    const char *name = strget(G_NAME);
    struct sb m = { msg, 0, sizeof msg };
    if (id == G_PARENT) {
        unsigned n = slen(path);
        while (n && path[n - 1] != '/') n--;
        path[n ? n - 1 : 0] = 0;
        files_refresh();
        return;
    }
    if (id == G_MKDIR) {
        if (!name[0]) { say("New folder: type its name first", 0); return; }
        child_path(p1, name);
        int e = cs(CS_MKDIR, file_req(fvol, p1, 0), BIG, 0);
        say(e ? "New folder: " : "Made the folder ", e ? cs_err(e) : name);
        if (!e) files_refresh();
        return;
    }
    if (fsel < 0) { say("Pick a file or folder in the list first", 0); return; }
    child_path(p1, fe[fsel].name);
    if (id == G_OPEN) {
        if (!(fe[fsel].flags & CSF_DIR)) { say("That is a file, not a folder", 0); return; }
        scpy(path, p1, CS_PATH);
        files_refresh();
    } else if (id == G_DELETE) {
        ss(&m, "Delete "); ss(&m, fe[fsel].name); ss(&m, (fe[fsel].flags & CSF_DIR) ? " (an empty folder)" : "");
        ss(&m, "\nfrom the SD card? This can't be undone.");
        if (!ask(VERSTR, msg, "Delete|Cancel")) return;
        int e = cs(CS_DELETE, file_req(fvol, p1, 0), BIG, 0);
        say(e ? "Delete: " : "Deleted ", e ? cs_err(e) : fe[fsel].name);
        if (!e) files_refresh();
    } else if (id == G_RENAME) {
        if (!name[0] || seq(name, fe[fsel].name)) { say("Rename: type the new name in Name first", 0); return; }
        child_path(p2, name);
        int e = cs(CS_RENAME, file_req(fvol, p1, p2), BIG, 0);
        say(e ? "Rename: " : "Renamed to ", e ? cs_err(e) : name);
        if (!e) files_refresh();
    } else if (id == G_BOOT) {
        if (!ends(fe[fsel].name, ".hdf")) { say("Boot from it: pick a hardfile (.hdf)", 0); return; }
        for (int i = 0; i < opt_count; i++)
            if (opt_items[i].kind == OPT_TEXTV && seq(opt_items[i].key, "hdf.boot")) {
                scpy(ov.text[i], fe[fsel].name, OPT_TEXT);
                unsigned n = slen(ov.text[i]);
                if (n > 4) ov.text[i][n - 4] = 0;              /* "Workbench.hdf": hdf.boot=Workbench */
                strset(G_OPT0 + i, ov.text[i]);
            }
        say(fe[fsel].name, " boots from the next start - Save, then Restart Pi");
    } else if (id == G_KICK) {
        if (fvol != CS_FAT || path[0]) { say("Use as Kickstart: the ROM must be in the root of the FAT partition", 0); return; }
        if (fe[fsel].flags & CSF_DIR) { say("Use as Kickstart: pick a ROM file", 0); return; }
        set_kickstart(fe[fsel].name);
        roms_scan();
        setattr(G_KS, GTCY_Labels, (ULONG)rom_labels); setattr(G_KS, GTCY_Active, (ULONG)(rom_cur < 0 ? 0 : rom_cur));
        say(fe[fsel].name, " is the Kickstart from the next start - Save, then Restart Pi");
    }
}

static volatile UBYTE *find_board(void)
{
    APTR eb = OpenLibrary("expansion.library", 36);
    if (!eb) return 0;
    struct ConfigDev *cd = FindConfigDev(eb, 0, AN_MANUFACTURER, AN_PRODUCT);
    CloseLibrary(eb);
    if (cd && ((volatile struct an_hdr *)cd->cd_BoardAddr)->magic == AN_MAGIC) return cd->cd_BoardAddr;
    return 0;
}

LONG card_main(const char *args, ULONG arglen)
{
    (void)args; (void)arglen;
    UBYTE *me = FindTask(0);
    APTR wbmsg = 0;
    if (!*(ULONG *)(me + 172)) {                    /* started from Workbench */
        lvo(SYSBASE, -384, 0, 0, (ULONG)(me + 92), 0);
        wbmsg = GetMsg(me + 92);
    }
    dos = OpenLibrary("dos.library", 37);
    if (!dos) { if (wbmsg) { Forbid(); ReplyMsg(wbmsg); } return 20; }
    out = Output(dos);
    intui = OpenLibrary("intuition.library", 37);
    gt = OpenLibrary("gadtools.library", 37);
    gfx = OpenLibrary("graphics.library", 37);
    LONG rc = 0;
    APTR scr = 0;
    if (!intui || !gt || !gfx) { put(VERSTR ": needs Kickstart 3 (gadtools.library 37)\n"); rc = 20; goto end; }
    big = AllocMem(BIG + 64, MEMF_PUBLIC | MEMF_CLEAR);
    M = AllocMem(sizeof *M, MEMF_PUBLIC | MEMF_CLEAR);
    if (M) {
        lv_files = (struct lv){ { 0, 0, 0, 0, 0 }, M->n_files, M->t_files, 0, MAXF };
        lv_lines = (struct lv){ { 0, 0, 0, 0, 0 }, M->n_lines, M->t_lines, 0, MAXL };
    }
    csport = CreateMsgPort();
    csio = csport ? CreateIORequest(csport, sizeof *csio) : 0;
    if (!big || !M || !csio) { put(VERSTR ": no memory\n"); rc = 20; goto end; }
    csopen = !OpenDevice("alice68hdf.device", CS_UNIT, csio, 0);
    if (!csopen) put(VERSTR ": alice68hdf.device's card service did not open: is this an Alice68 kernel?\n");
    for (int i = 0; i < G_N; i++) gpage[i] = -1;
    load_all();

    scr = LockPubScreen(0);
    if (!scr) { rc = 20; goto end; }
    vi = GetVisualInfoA(scr, 0);
    UBYTE *s = scr;
    x0 = s[36] + 4; y0 = s[35] + (*(UWORD *)(*(UBYTE **)(s + 40) + 4)) + 1 + 4;
    if (!make_gadgets()) { put(VERSTR ": the gadgets could not be made\n"); rc = 20; goto end; }
    struct TagItem wt[] = {
        { WA_(19), 632 }, { WA_(20), 248 }, { WA_(9), (ULONG)glist_top }, { WA_(11), (ULONG)VERSTR " - the SD card" },
        { WA_(31), 1 }, { WA_(32), 1 }, { WA_(33), 1 }, { WA_(38), 1 }, { WA_(42), 1 }, { WA_(22), (ULONG)scr },
        { WA_(7), IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS },
        { TAG_DONE, 0 } };
    win = OpenWindowTagList(wt);
    UnlockPubScreen(scr); scr = 0;
    if (!win) { put(VERSTR ": the window would not open\n"); rc = 20; goto end; }
    GT_RefreshWindow(win);
    show_page(0);
    card_show(0);
    files_refresh();
    text_page_show();
    help_show("Pick a setting to read what it does. Save writes cmdline.txt and config.txt; Restart Pi starts again with them.");
    say(csopen ? "Ready. Nothing is written to the card until you press Save (or change files on the Files page)." :
                 "alice68hdf.device's card service is not there: this needs an Alice68 kernel with it", 0);

    ULONG wmask = 1u << ((struct MsgPort *)*(APTR *)(win + 86))->mp_SigBit;
    int quit = 0;
    while (!quit) {
        ULONG got = Wait(wmask | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C) quit = 1;
        UBYTE *m;
        while ((m = GT_GetIMsg(*(APTR *)(win + 86)))) {
            ULONG cls = *(ULONG *)(m + 20); UWORD code = *(UWORD *)(m + 24); UBYTE *ia = *(UBYTE **)(m + 28);
            ULONG secs = *(ULONG *)(m + 36), micros = *(ULONG *)(m + 40);
            GT_ReplyIMsg(m);
            if (cls == IDCMP_CLOSEWINDOW) {
                if (!changed() || ask(VERSTR, "cmdline.txt or config.txt has changes that are not saved.\nLeave without saving them?", "Leave|Stay")) quit = 1;
            } else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); page_frame(); GT_EndRefresh(win, 1); }
            else if (cls == IDCMP_GADGETUP || cls == IDCMP_GADGETDOWN) {
                UWORD id = *(UWORD *)(ia + 38);
                if (id >= G_TAB0 && id <= G_TAB3) {
                    show_page(id - G_TAB0);
                    if (id == G_TAB3) text_page_show();
                } else if (id == G_SAVE) save_all();
                else if (id == G_RESTART) {
                    volatile UBYTE *b = find_board();
                    if (!b) say("No Alice68 network board: restart the Pi by hand (or Ctrl + Left Shift + F11)", 0);
                    else if (!changed() || ask(VERSTR, "cmdline.txt or config.txt has changes that are not saved.\nRestart without them?", "Restart|Cancel")) {
                        say("Restarting the Pi...", 0);
                        ((volatile struct an_hdr *)b)->restart = AN_RESTART;
                    }
                } else if (id == G_FREE) { say("Counting free space...", 0); card_show(1); say("Free space counted", 0); }
                else if (id == G_VOL) { fvol = code ? CS_EXFAT : CS_FAT; path[0] = 0; files_refresh(); }
                else if (id == G_FILTER) { filter = code; files_show(); }
                else if (id == G_FILES) files_click(code, secs, micros);
                else if (id >= G_PARENT && id <= G_MKDIR && id != G_PATH && id != G_FILES) files_action(id);
                else if (id == G_KS) {
                    if (code < nroms && roms[code][0] != '(') { set_kickstart(roms[code]); rom_cur = code; say(roms[code], " is the Kickstart from the next start (Save)"); }
                } else if (id == G_REVERT) {
                    load_all(); opts_show(); text_page_show();
                    setattr(G_KS, GTCY_Labels, (ULONG)rom_labels); setattr(G_KS, GTCY_Active, (ULONG)(rom_cur < 0 ? 0 : rom_cur));
                    say("Back to cmdline.txt and config.txt as on the card", 0);
                } else if (id >= G_OPT0 && id < G_OPT0 + opt_count) {
                    int i = opt_gadget_item[id];
                    const struct opt_item *it = &opt_items[i];
                    help_show(it->help);
                    if (it->kind == OPT_CHOICE) { if (code < opt_nchoices(it)) ov.cur[i] = (UBYTE)code; }
                    else {
                        const char *v = strget(id);
                        unsigned k = 0;
                        for (; v[k] && k + 1 < OPT_TEXT; k++) {
                            char c = v[k];
                            if ((it->flags & OT_UPPER) && c >= 'a' && c <= 'z') c = (char)(c - 32);
                            if ((it->flags & OT_QUOTE) && c == '"') c = '\'';
                            ov.text[i][k] = c;
                        }
                        ov.text[i][k] = 0;
                        if (!seq(v, ov.text[i])) strset(id, ov.text[i]);
                    }
                    if (opt_item_changed(&ov, i)) { char w[96]; opt_written(&ov, i, w, sizeof w); say("cmdline.txt will have: ", w[0] ? w : "(the default: no option)"); }
                } else if (id == G_LINES) {
                    if (code < nlines) { lsel = code; strset(G_LINE, lines[code]); }
                } else if (id == G_LINE) {
                    if (lsel >= 0 && lsel < nlines) { scpy(lines[lsel], strget(G_LINE), 200); lines_show(); roms_scan(); say("config.txt line changed (Save to write it)", 0); }
                    else say("Pick a line first, or Add one", 0);
                } else if (id == G_ADDL) {
                    if (nlines >= MAXL) say("config.txt has too many lines", 0);
                    else {
                        int at = lsel >= 0 ? lsel + 1 : nlines;
                        for (int i = nlines; i > at; i--) scpy(lines[i], lines[i - 1], 200);
                        scpy(lines[at], strget(G_LINE), 200);
                        nlines++; lsel = at; lines_show(); roms_scan();
                        say("Line added to config.txt (Save to write it)", 0);
                    }
                } else if (id == G_DELL) {
                    if (lsel < 0 || lsel >= nlines) say("Pick the line to delete first", 0);
                    else {
                        for (int i = lsel; i + 1 < nlines; i++) scpy(lines[i], lines[i + 1], 200);
                        nlines--; lsel = -1; strset(G_LINE, ""); lines_show(); roms_scan();
                        say("Line deleted from config.txt (Save to write it)", 0);
                    }
                } else if (id == G_CMD) {
                    scpy(cmd_base, strget(G_CMD), sizeof cmd_base);
                    opt_init(&ov, cmd_base);
                    opts_show();
                    say("cmdline.txt taken as typed; the Settings page follows it (Save to write it)", 0);
                }
            }
        }
        if (page == 3) textset(G_TNOTE, changed() ? "Changes not saved yet: Save writes them (the old files are kept as .bak)" : "As on the card");
    }
end:
    if (win) {
        if (page >= 0 && glist_page[page]) RemoveGList(win, glist_page[page], -1);
        CloseWindow(win);
    }
    if (glist_top) FreeGadgets(glist_top);
    for (int i = 0; i < 4; i++) if (glist_page[i]) FreeGadgets(glist_page[i]);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(scr);
    if (csopen) CloseDevice(csio);
    if (csio) DeleteIORequest(csio);
    if (csport) DeleteMsgPort(csport);
    if (big) FreeMem(big, BIG + 64);
    if (M) FreeMem(M, sizeof *M);
    if (gfx) CloseLibrary(gfx);
    if (gt) CloseLibrary(gt);
    if (intui) CloseLibrary(intui);
    CloseLibrary(dos);
    if (wbmsg) { Forbid(); ReplyMsg(wbmsg); }
    return rc;
}
