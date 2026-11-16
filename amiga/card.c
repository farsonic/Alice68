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

#define VERSTR "Alice68-Card 1.1"
#define PMASK  "********"                     /* a WiFi password as shown: never the password itself */
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
#define IDCMP_NEWSIZE       0x2u
#define IDCMP_REFRESHWINDOW 0x4u
#define IDCMP_MOUSEBUTTONS  0x8u
#define IDCMP_MOUSEMOVE     0x10u
#define IDCMP_GADGETDOWN    0x20u
#define IDCMP_GADGETUP      0x40u
#define IDCMP_CLOSEWINDOW   0x200u
#define IDCMP_VANILLAKEY    0x200000u
#define IDCMP_INTUITICKS    0x400000u

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
    /* Bluetooth */
    G_TAB4, G_BSTAT, G_BPAIRED, G_BFOUND, G_BSCAN, G_BPAIR, G_BCONN, G_BFORGET, G_BKEY, G_BNOTE, G_BLOG,
    G_OPT0                                           /* G_OPT0 + i: option i (alice_opts.c) */
};
#define G_N (G_OPT0 + OPT_MAX)
static APTR gad[G_N];
static int gpage[G_N];                               /* the page a gadget is on, -1 always there */
static UBYTE *win;
static APTR vi;
static APTR glist_top, glist_page[5];
static int page = -1;
static int x0, y0;
static int W = 616, H = 240;                 /* the room inside the borders (make_gadgets) */

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
static char cmd_file[1600];                          /* cmdline.txt as on the card */
static char cmd_base[1600];                          /* what the options apply to (the file, or the raw line typed) */
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
        else strset(g, opt_items[i].secret && ov.text[i][0] ? PMASK : ov.text[i]);
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

/* ---- the WiFi password: shown as PMASK, never as it is -------------------------------------- */
static int secret_item(void) { for (int i = 0; i < opt_count; i++) if (opt_items[i].secret) return i; return -1; }
static int at_psk(const char *line, const char *p)
{
    static const char k[] = "wifi.psk=";
    if (p != line && p[-1] != ' ') return 0;
    for (int i = 0; k[i]; i++) if (p[i] != k[i]) return 0;
    return 1;
}
/* a command line with wifi.psk='s value replaced by PMASK */
static void psk_mask(const char *in, char *out, unsigned max)
{
    struct sb b = { out, 0, max };
    out[0] = 0;
    for (const char *p = in; *p; ) {
        if (at_psk(in, p)) {
            ss(&b, "wifi.psk=" PMASK);
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
/* the reverse, for a line typed in: wifi.psk=PMASK stands for the password as it is now */
static void psk_unmask(const char *in, char *out, unsigned max)
{
    char real[96] = "";
    int si = secret_item();
    if (si >= 0) opt_written(&ov, si, real, sizeof real);
    struct sb b = { out, 0, max };
    out[0] = 0;
    for (const char *p = in; *p; ) {
        if (at_psk(in, p)) {
            const char *v = p + 9;
            int q = *v == '"';
            const char *m = v + q;
            int masked = 1;
            for (int i = 0; PMASK[i]; i++) if (m[i] != PMASK[i]) masked = 0;
            if (masked && (q ? m[8] == '"' : !m[8] || m[8] == ' ')) {
                ss(&b, real);
                p = m + 8 + q;
                continue;
            }
        }
        sc(&b, *p++);
    }
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
    psk_mask(cmd_out, shown_cmd, sizeof shown_cmd);
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

static volatile UBYTE *find_board(void);
/* ---- Bluetooth: the kernel's, through the network board (alice_net.h struct an_bt) ------------ */
static volatile UBYTE *nb;                           /* the network board */
static ULONG bt_gen_seen, bt_sig;
static int bt_psel = -1, bt_fsel = -1;
static struct Node_ n_bp[AN_BT_KEYS], n_bf[AN_BT_FOUND];
static char t_bp[AN_BT_KEYS][80], t_bf[AN_BT_FOUND][80];
static struct lv lv_bp = { { 0, 0, 0, 0, 0 }, n_bp, t_bp, 0, AN_BT_KEYS };
static struct lv lv_bf = { { 0, 0, 0, 0, 0 }, n_bf, t_bf, 0, AN_BT_FOUND };
#define BLOG_N 8
static struct Node_ n_bl[BLOG_N];
static char t_bl[BLOG_N][80];
static struct lv lv_bl = { { 0, 0, 0, 0, 0 }, n_bl, t_bl, 0, BLOG_N };
static ULONG blog_seen = ~0u;
/* the network board's log ring: its last [BT] lines (the kernel's Bluetooth writes them there) */
static void bt_log_show(void)
{
    if (!nb) return;
    ULONG wr = ((volatile struct an_hdr *)nb)->log_wr;
    if (wr == blog_seen) return;
    blog_seen = wr;
    volatile const char *r = (volatile const char *)(nb + AN_OFF_LOG);
    ULONG start = wr > AN_LOG_BYTES ? wr - AN_LOG_BYTES : 0;
    static char ln[BLOG_N][80];
    int nl = 0;
    char cur[80]; unsigned n = 0;
    for (ULONG i = start; i != wr; i++) {
        char c = r[i % AN_LOG_BYTES];
        if (c != '\n') { if (n < 79) cur[n++] = c; continue; }
        cur[n] = 0;
        if (n > 4 && cur[0] == '[' && cur[1] == 'B' && cur[2] == 'T' && cur[3] == ']') {
            if (nl == BLOG_N) { for (int k = 1; k < BLOG_N; k++) scpy(ln[k - 1], ln[k], 80); nl--; }
            scpy(ln[nl++], cur + 5, 80);
        }
        n = 0;
    }
    lv_begin(&lv_bl, G_BLOG);
    for (int k = 0; k < nl; k++) { char *t = lv_add(&lv_bl); scpy(t, ln[k], 80); }
    lv_end(&lv_bl, G_BLOG);
}
static char bstat[120], bkey[120];
static volatile struct an_bt *btb(void) { return nb ? (volatile struct an_bt *)(nb + AN_OFF_BT) : 0; }
static void saddr(struct sb *b, const volatile unsigned char *a)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 5; i >= 0; i--) { sc(b, hx[a[i] >> 4]); sc(b, hx[a[i] & 15]); if (i) sc(b, ':'); }
}
static ULONG bt_signature(void)
{
    volatile struct an_bt *x = btb();
    if (!x) return 0;
    ULONG h = x->state * 7 + x->nkeys * 131 + x->nfound * 977 + x->scanning * 3 + x->passkey + x->passkey_shown * 5 + x->keys_gen * 11;
    for (int i = 0; i < 64 && x->msg[i]; i++) h = h * 31 + (UBYTE)x->msg[i];
    for (ULONG i = 0; i < x->nkeys && i < AN_BT_KEYS; i++) h = h * 17 + x->keys[i].flags;
    for (ULONG i = 0; i < x->nfound && i < AN_BT_FOUND; i++) h = h * 13 + (UBYTE)x->found[i].name[0] + x->found[i].addr[0];
    return h;
}
static void bt_show(void)
{
    volatile struct an_bt *x = btb();
    struct sb b = { bstat, 0, sizeof bstat };
    if (!x) { ss(&b, "No Alice68 network board: Bluetooth needs an Alice68 kernel"); textset(G_BSTAT, bstat); return; }
    static const char *const st[] = { "off", "waiting for its firmware (BCM4345C0.hcd in the WIFI folder)", "starting", "ready", "FAILED" };
    ss(&b, x->state < 5 ? st[x->state] : "?");
    char m[65];
    for (int i = 0; i < 64; i++) { m[i] = x->msg[i]; if (!m[i]) break; }
    m[64] = 0;
    if (m[0] && x->state != 1) { ss(&b, ": "); ss(&b, m); }
    textset(G_BSTAT, bstat);
    lv_begin(&lv_bp, G_BPAIRED);
    for (ULONG i = 0; i < x->nkeys && i < AN_BT_KEYS; i++) {
        char *t = lv_add(&lv_bp);
        struct sb l = { t, 0, 80 };
        volatile struct an_btkey *k = &x->keys[i];
        saddr(&l, k->addr); spad(&l, 19);
        ss(&l, k->flags & AN_BTK_PAD ? "game pad" : k->flags & AN_BTK_KEYBOARD ? (k->flags & AN_BTK_MOUSE ? "keyboard+mouse" : "keyboard") :
               k->flags & AN_BTK_MOUSE ? "mouse" : "device");
        spad(&l, 34);
        char nm[41]; for (int j = 0; j < 40; j++) { nm[j] = k->name[j]; if (!nm[j]) break; } nm[40] = 0;
        ss(&l, nm); spad(&l, 50);
        if (k->flags & AN_BTK_CONNECTED) ss(&l, "connected");
    }
    lv_end(&lv_bp, G_BPAIRED);
    lv_begin(&lv_bf, G_BFOUND);
    for (ULONG i = 0; i < x->nfound && i < AN_BT_FOUND; i++) {
        char *t = lv_add(&lv_bf);
        struct sb l = { t, 0, 80 };
        volatile struct an_btfound *f = &x->found[i];
        saddr(&l, f->addr); spad(&l, 19);
        ULONG m2 = (f->cod >> 6) & 3, sub = (f->cod >> 2) & 15;         /* sub: 1 joystick, 2 game pad */
        ss(&l, sub == 2 ? "game pad" : sub == 1 ? "joystick" : m2 == 3 ? "keyboard+mouse" : m2 == 1 ? "keyboard" : m2 == 2 ? "mouse" : "device");
        if (f->cod & 0x80000000u) ss(&l, " (LE)");             /* Bluetooth LE */
        spad(&l, 34);
        char nm[41]; for (int j = 0; j < 40; j++) { nm[j] = f->name[j]; if (!nm[j]) break; } nm[40] = 0;
        ss(&l, nm[0] ? nm : "(no name yet)");
    }
    lv_end(&lv_bf, G_BFOUND);
    b = (struct sb){ bkey, 0, sizeof bkey };
    if (x->passkey_shown) { ss(&b, "Type "); char d[7]; ULONG v = x->passkey; for (int i = 5; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; } d[6] = 0; ss(&b, d); ss(&b, " on the Bluetooth keyboard, then Enter"); }
    else if (x->scanning) ss(&b, "Scanning for 10 seconds: put the keyboard, mouse or pad in pairing mode now");
    else if (x->pairing) ss(&b, "Pairing...");
    textset(G_BKEY, bkey);
    textset(G_BNOTE, "Paired devices are kept in cmdline.txt (bt.dev=...) and reconnect by themselves once AmigaOS starts.");
    blog_seen = ~0u;
    bt_log_show();
}
static void bt_cmd(ULONG cmd, const volatile unsigned char *addr)
{
    volatile struct an_bt *x = btb();
    if (!x) { say("No Alice68 network board", 0); return; }
    if (x->state != AN_BT_READY) { say("Bluetooth is not ready: ", x->state == AN_BT_NOFW ? "its firmware has not arrived" : "see the line above"); return; }
    for (int i = 0; i < 6; i++) x->cmd_addr[i] = addr ? addr[i] : 0;
    x->cmd = cmd;
    x->cmd_seq = x->cmd_seq + 1;
}
/* the paired list changed: bt.dev= in cmdline.txt, at once (pairing would be lost otherwise) */
static void bt_save_keys(void)
{
    volatile struct an_bt *x = btb();
    if (!x) return;
    static char ed[1000];                            /* six LE devices with IRKs: about 700 */
    struct sb b = { ed, 0, sizeof ed };
    static const char hx[] = "0123456789abcdef";
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
    static char t[1600];
    if (cmd_apply(cmd_file, ed, t, sizeof t) < 0) { say("Bluetooth: cmdline.txt would be too long to keep the pairing", 0); return; }
    int e = write_text("cmdline.txt", t, slen(t));
    if (e) { say("Bluetooth: the pairing could not be kept in cmdline.txt: ", cs_err(e)); return; }
    scpy(cmd_file, t, sizeof cmd_file);
    if (cmd_apply(cmd_base, ed, t, sizeof t) >= 0) { scpy(cmd_base, t, sizeof cmd_base); for (char *p = cmd_base; *p; p++) if (*p == '\n') *p = ' '; }
    say("Bluetooth: the paired devices are kept in cmdline.txt", 0);
}
static void bt_tick(void)
{
    volatile struct an_bt *x = btb();
    if (!x) return;
    if (x->keys_gen != bt_gen_seen) { bt_gen_seen = x->keys_gen; bt_save_keys(); }
    ULONG sg = bt_signature();
    if (page == 4 && sg != bt_sig) { bt_sig = sg; bt_show(); }
    else if (page == 4) bt_log_show();
}

/* ---- the window ----------------------------------------------------------------------- */
static void page_frame(void)
{
    APTR rp = *(APTR *)(win + 50);
    struct TagItem t[] = { { GT_VisualInfo, (ULONG)vi }, { GTBB_Recessed, 1 }, { TAG_DONE, 0 } };
    DrawBevelBoxA(rp, x0, y0 + 16, W, H - 16 - 16 + 1, t);
}
static void show_page(int p)
{
    if (p == page) return;
    APTR rp = *(APTR *)(win + 50);
    if (page >= 0 && glist_page[page]) RemoveGList(win, glist_page[page], -1);
    page = p;
    SetAPen(rp, 0);
    RectFill(rp, x0, y0 + 16, x0 + W - 1, y0 + H - 15);
    if (glist_page[page]) { AddGList(win, glist_page[page], -1, -1); RefreshGList(glist_page[page], win, -1); }
    GT_RefreshWindow(win);
    page_frame();
}

static UWORD scr_w = 640, scr_h = 256;
static LONG WindowLimits(APTR w, LONG a, LONG b, ULONG c, ULONG d) { return (LONG)LY(intui, -318, (ULONG)a, (ULONG)b, c, d, w, 0, 0, 0); }
static void RefreshWindowFrame(APTR w) { LY(intui, -456, 0, 0, 0, 0, w, 0, 0, 0); }
static int make_gadgets(void);
static void text_page_show(void);
static void bt_show(void);
static void dims_from_window(void)
{
    UBYTE *w = win;
    int ww = *(WORD *)(w + 8), wh = *(WORD *)(w + 10), bl = w[54], bt = w[55], br = w[56], bb = w[57];
    x0 = bl + 4; y0 = bt + 4;
    W = ww - bl - br - 12; H = wh - bt - bb - 8;
    if (W < 560) W = 560;
    if (H < 224) H = 224;
}
/* every gadget's contents again, after a rebuild */
static void refill_all(void)
{
    textset(G_STATUS, status); textset(G_CARD, cardline); textset(G_EXFAT, exline);
    textset(G_PATH, pathline); textset(G_FINFO, finfo);
    for (int k = 0; k < 3; k++) textset(G_HELP0 + k, helpl[k]);
    if (lv_parts.l.head) setattr(G_PARTS, GTLV_Labels, (ULONG)&lv_parts.l);
    if (lv_used.l.head) setattr(G_USED, GTLV_Labels, (ULONG)&lv_used.l);
    if (lv_files.l.head) setattr(G_FILES, GTLV_Labels, (ULONG)&lv_files.l);
    text_page_show();
    if (nb) bt_show();
}
/* the window has a new size: the gadgets made again to fit it */
static void relayout(void)
{
    UBYTE *w = win;
    APTR rp = *(APTR *)(w + 50);
    /* the page first: RemoveGList(.., -1) takes a list with everything after it still linked
       on, and FreeGadgets frees along that chain - the page's gadgets would be freed twice */
    if (page >= 0 && glist_page[page]) RemoveGList(win, glist_page[page], -1);
    RemoveGList(win, glist_top, -1);
    FreeGadgets(glist_top); glist_top = 0;
    for (int i = 0; i < 5; i++) { if (glist_page[i]) FreeGadgets(glist_page[i]); glist_page[i] = 0; }
    for (int i = 0; i < G_N; i++) gad[i] = 0;
    dims_from_window();
    int ok = make_gadgets();
    SetAPen(rp, 0);
    RectFill(rp, w[54], w[55], *(WORD *)(w + 8) - w[56] - 1, *(WORD *)(w + 10) - w[57] - 1);
    if (!ok) { put(VERSTR ": the gadgets could not be made again\n"); return; }
    AddGList(win, glist_top, -1, -1);
    if (glist_page[page]) AddGList(win, glist_page[page], -1, -1);
    RefreshWindowFrame(win);
    GT_RefreshWindow(win);
    page_frame();
    refill_all();
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
/* everything placed from the window's size (W x H inside the borders): resizing rebuilds it */

static int bw_(const char *t) { return (int)slen(t) * 8 + 20; }
static int make_gadgets(void)
{
    struct TagItem txt[] = { { GTTX_Border, 1 }, { TAG_DONE, 0 } };
    struct TagItem ro[] = { { GTLV_ReadOnly, 1 }, { TAG_DONE, 0 } };
    struct TagItem sel[] = { { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };
    static const char *const tabs[] = { "Card", "Files", "Settings", "Text files", "Bluetooth" };
    const int PY = 18, PB = H - 16;                 /* the page: from under the tabs to over the status line */
    APTR g = CreateContext(&glist_top);
    int x = 0;
    for (int i = 0; i < 5; i++) { int w = bw_(tabs[i]); g = mk(g, -1, BUTTON_KIND, i < 4 ? G_TAB0 + i : G_TAB4, x, 0, w, 14, tabs[i], PLACETEXT_IN, 0); x += w + 2; }
    int rw = bw_("Restart Pi"), sw = bw_("Save") + 16;
    g = mk(g, -1, BUTTON_KIND, G_RESTART, W - rw, 0, rw, 14, "Restart Pi", PLACETEXT_IN, 0);
    g = mk(g, -1, BUTTON_KIND, G_SAVE, W - rw - 4 - sw, 0, sw, 14, "Save", PLACETEXT_IN, 0);
    g = mk(g, -1, TEXT_KIND, G_STATUS, 0, H - 12, W, 12, 0, 0, txt);
    if (!g) return 0;

    /* Card: the partitions and what is mounted, each list half of what is left */
    g = CreateContext(&glist_page[0]);
    int lh = (PB - PY - 72) / 2;
    if (lh < 32) lh = 32;
    g = mk(g, 0, TEXT_KIND, G_CARD, 8, PY + 6, W - 16, 12, 0, 0, txt);
    g = mk(g, 0, LISTVIEW_KIND, G_PARTS, 8, PY + 22, W - 16, lh, 0, 0, ro);
    g = mk(g, 0, TEXT_KIND, G_EXFAT, 8, PY + 26 + lh, W - 16, 12, 0, 0, txt);
    g = mk(g, 0, LISTVIEW_KIND, G_USED, 8, PY + 42 + lh, W - 16, PB - 22 - (PY + 42 + lh), 0, 0, ro);
    g = mk(g, 0, BUTTON_KIND, G_FREE, 8, PB - 18, bw_("Count free space"), 14, "Count free space", PLACETEXT_IN, 0);
    if (!g) return 0;

    /* Files: the list on the left, the buttons in a column on the right */
    g = CreateContext(&glist_page[1]);
    int bw = bw_("Use as Kickstart"), rx = W - 8 - bw, lw = rx - 16;
    struct TagItem cy1[] = { { GTCY_Labels, (ULONG)vol_names }, { GTCY_Active, fvol }, { TAG_DONE, 0 } };
    struct TagItem cy2[] = { { GTCY_Labels, (ULONG)filter_names }, { GTCY_Active, (ULONG)filter }, { TAG_DONE, 0 } };
    int pw = bw_("Parent"), cw = (lw - pw - 16) / 2;
    if (cw > 170) cw = 170;
    g = mk(g, 1, CYCLE_KIND, G_VOL, 8, PY + 4, cw, 14, 0, 0, cy1);
    g = mk(g, 1, CYCLE_KIND, G_FILTER, 16 + cw, PY + 4, cw, 14, 0, 0, cy2);
    g = mk(g, 1, BUTTON_KIND, G_PARENT, 8 + lw - pw, PY + 4, pw, 14, "Parent", PLACETEXT_IN, 0);
    g = mk(g, 1, TEXT_KIND, G_PATH, 8, PY + 22, lw, 12, 0, 0, txt);
    int ly = PY + 38, lhf = PB - ly - 36;
    g = mk(g, 1, LISTVIEW_KIND, G_FILES, 8, ly, lw, lhf, 0, 0, sel);
    g = mk(g, 1, BUTTON_KIND, G_OPEN, rx, PY + 4, bw, 14, "Open folder", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_BOOT, rx, ly, bw, 14, "Boot from it", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_KICK, rx, ly + 16, bw, 14, "Use as Kickstart", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_RENAME, rx, ly + 40, bw, 14, "Rename to name", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_MKDIR, rx, ly + 56, bw, 14, "New folder", PLACETEXT_IN, 0);
    g = mk(g, 1, BUTTON_KIND, G_DELETE, rx, ly + 80, bw, 14, "Delete...", PLACETEXT_IN, 0);
    struct TagItem st[] = { { GTST_MaxChars, 107 }, { TAG_DONE, 0 } };
    g = mk(g, 1, STRING_KIND, G_NAME, 56, PB - 32, lw - 48, 14, "Name", PLACETEXT_LEFT, st);
    g = mk(g, 1, TEXT_KIND, G_FINFO, 8, PB - 15, W - 16, 12, 0, 0, 0);
    if (!g) return 0;

    /* Settings: Display and Storage on the left, Network and USB on the right, Kickstart under them */
    g = CreateContext(&glist_page[2]);
    const int L = 16 * 8 + 4;                       /* room for the longest label */
    int gw = (W - 2 * L - 24) / 2;
    if (gw > 230) gw = 230;
    int row[2] = { 0, 0 };
    for (int i = 0; i < opt_count && i < OPT_MAX; i++) {
        const struct opt_item *it = &opt_items[i];
        int col = it->sec >= 2, y = PY + 4 + row[col]++ * 14, gx = col ? W / 2 + L : 8 + L;
        int id = G_OPT0 + i;
        opt_gadget_item[id] = i;
        if (it->kind == OPT_CHOICE) {
            struct TagItem ct[] = { { GTCY_Labels, (ULONG)it->names }, { GTCY_Active, ov.cur[i] }, { TAG_DONE, 0 } };
            g = mk(g, 2, CYCLE_KIND, id, gx, y, gw, 13, it->label, PLACETEXT_LEFT, ct);
        } else {
            struct TagItem tt[] = { { GTST_String, (ULONG)(it->secret && ov.text[i][0] ? PMASK : ov.text[i]) }, { GTST_MaxChars, it->maxlen }, { TAG_DONE, 0 } };
            g = mk(g, 2, STRING_KIND, id, gx, y, gw, 13, it->label, PLACETEXT_LEFT, tt);
        }
    }
    struct TagItem kt[] = { { GTCY_Labels, (ULONG)rom_labels }, { GTCY_Active, (ULONG)(rom_cur < 0 ? 0 : rom_cur) }, { TAG_DONE, 0 } };
    g = mk(g, 2, CYCLE_KIND, G_KS, 8 + L, PY + 4 + row[0] * 14, gw, 13, "Kickstart ROM", PLACETEXT_LEFT, kt);
    int rows = (row[0] + 1 > row[1] ? row[0] + 1 : row[1]), hy = PY + 8 + rows * 14;
    for (int k = 0; k < 3; k++) g = mk(g, 2, TEXT_KIND, G_HELP0 + k, 8, hy + k * 12, W - 16, 12, 0, 0, 0);
    g = mk(g, 2, BUTTON_KIND, G_REVERT, 8, PB - 18, bw_("Back to the card's"), 14, "Back to the card's", PLACETEXT_IN, 0);
    if (!g) return 0;

    /* Text files: config.txt's lines take what is left */
    g = CreateContext(&glist_page[3]);
    int aw = bw_("Delete"), cl = 12 * 8 + 8;
    g = mk(g, 3, LISTVIEW_KIND, G_LINES, 8, PY + 4, W - 16, PB - PY - 4 - 58, 0, 0, sel);
    struct TagItem ls[] = { { GTST_MaxChars, 199 }, { TAG_DONE, 0 } };
    g = mk(g, 3, STRING_KIND, G_LINE, 8, PB - 50, W - 24 - 2 * aw - 4, 14, 0, 0, ls);
    g = mk(g, 3, BUTTON_KIND, G_ADDL, W - 8 - 2 * aw - 4, PB - 50, aw, 14, "Add", PLACETEXT_IN, 0);
    g = mk(g, 3, BUTTON_KIND, G_DELL, W - 8 - aw, PB - 50, aw, 14, "Delete", PLACETEXT_IN, 0);
    struct TagItem cs_[] = { { GTST_MaxChars, 1000 }, { TAG_DONE, 0 } };
    g = mk(g, 3, STRING_KIND, G_CMD, 8 + cl, PB - 32, W - 16 - cl, 14, "cmdline.txt", PLACETEXT_LEFT, cs_);
    g = mk(g, 3, TEXT_KIND, G_TNOTE, 8, PB - 15, W - 16, 12, 0, 0, 0);
    if (!g) return 0;

    /* Bluetooth: paired above, found below, the buttons beside them */
    g = CreateContext(&glist_page[4]);
    int bb = bw_("Connect now") + 24, bx = W - 8 - bb, bl = bx - 16, bh = (PB - PY - 74 - 50) / 2;
    if (bh < 28) bh = 28;
    g = mk(g, 4, TEXT_KIND, G_BSTAT, 8, PY + 4, W - 16, 12, 0, 0, txt);
    g = mk(g, 4, LISTVIEW_KIND, G_BPAIRED, 8, PY + 20, bl, bh, 0, 0, sel);
    g = mk(g, 4, BUTTON_KIND, G_BCONN, bx, PY + 20, bb, 14, "Connect now", PLACETEXT_IN, 0);
    g = mk(g, 4, BUTTON_KIND, G_BFORGET, bx, PY + 36, bb, 14, "Forget...", PLACETEXT_IN, 0);
    int fy = PY + 26 + bh;
    g = mk(g, 4, LISTVIEW_KIND, G_BFOUND, 8, fy, bl, bh, 0, 0, sel);
    g = mk(g, 4, BUTTON_KIND, G_BSCAN, bx, fy, bb, 14, "Scan", PLACETEXT_IN, 0);
    g = mk(g, 4, BUTTON_KIND, G_BPAIR, bx, fy + 16, bb, 14, "Pair", PLACETEXT_IN, 0);
    g = mk(g, 4, LISTVIEW_KIND, G_BLOG, 8, fy + bh + 6, W - 16, PB - 34 - (fy + bh + 6), 0, 0, ro);   /* the last [BT] log lines */
    g = mk(g, 4, TEXT_KIND, G_BKEY, 8, PB - 32, W - 16, 12, 0, 0, txt);
    g = mk(g, 4, TEXT_KIND, G_BNOTE, 8, PB - 15, W - 16, 12, 0, 0, 0);
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
    nb = find_board();
    if (btb()) bt_gen_seen = btb()->keys_gen;

    scr = LockPubScreen(0);
    if (!scr) { rc = 20; goto end; }
    vi = GetVisualInfoA(scr, 0);
    UBYTE *s = scr;
    scr_w = *(UWORD *)(s + 12); scr_h = *(UWORD *)(s + 14);
    x0 = s[36] + 4; y0 = s[35] + (*(UWORD *)(*(UBYTE **)(s + 40) + 4)) + 1 + 4;
    if (!make_gadgets()) { put(VERSTR ": the gadgets could not be made\n"); rc = 20; goto end; }
    struct TagItem wt[] = {
        { WA_(19), 628 }, { WA_(20), 248 }, { WA_(30), 1 }, { WA_(44), 1 }, { WA_(9), (ULONG)glist_top }, { WA_(11), (ULONG)VERSTR " - the SD card" },
        { WA_(31), 1 }, { WA_(32), 1 }, { WA_(33), 1 }, { WA_(38), 1 }, { WA_(42), 1 }, { WA_(22), (ULONG)scr },
        { WA_(7), IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_INTUITICKS | IDCMP_NEWSIZE },
        { TAG_DONE, 0 } };
    win = OpenWindowTagList(wt);
    UnlockPubScreen(scr); scr = 0;
    if (!win) { put(VERSTR ": the window would not open\n"); rc = 20; goto end; }
    {
        UBYTE *w = win;
        WindowLimits(win, 560 + 12 + w[54] + w[56], 224 + 8 + w[55] + w[57], scr_w, scr_h);
    }
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
            else if (cls == IDCMP_NEWSIZE) relayout();
            else if (cls == IDCMP_GADGETUP || cls == IDCMP_GADGETDOWN) {
                UWORD id = *(UWORD *)(ia + 38);
                if (id >= G_TAB0 && id <= G_TAB3) {
                    show_page(id - G_TAB0);
                    if (id == G_TAB3) text_page_show();
                } else if (id == G_TAB4) { show_page(4); bt_sig = bt_signature(); bt_show(); }
                else if (id == G_BSCAN) { bt_cmd(AN_BT_CMD_SCAN, 0); bt_fsel = -1; }
                else if (id == G_BFOUND) bt_fsel = code;
                else if (id == G_BPAIRED) bt_psel = code;
                else if (id == G_BPAIR) {
                    volatile struct an_bt *x = btb();
                    if (!x || bt_fsel < 0 || (ULONG)bt_fsel >= x->nfound) say("Pair: Scan, then pick the device in the lower list", 0);
                    else { bt_cmd(AN_BT_CMD_PAIR, x->found[bt_fsel].addr); say("Pairing - follow what the line above the note says", 0); }
                } else if (id == G_BCONN || id == G_BFORGET) {
                    volatile struct an_bt *x = btb();
                    if (!x || bt_psel < 0 || (ULONG)bt_psel >= x->nkeys) say("Pick a paired device first", 0);
                    else if (id == G_BCONN) bt_cmd(AN_BT_CMD_CONNECT, x->keys[bt_psel].addr);
                    else if (ask(VERSTR, "Forget this Bluetooth device?\nIt has to be paired again to be used.", "Forget|Cancel")) { bt_cmd(AN_BT_CMD_FORGET, x->keys[bt_psel].addr); bt_psel = -1; }
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
                    else if (it->secret && seq(strget(id), PMASK)) ;       /* the password left as it was */
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
                        if (it->secret) strset(id, ov.text[i][0] ? PMASK : "");
                        else if (!seq(v, ov.text[i])) strset(id, ov.text[i]);
                    }
                    if (opt_item_changed(&ov, i)) {
                        char w[96];
                        opt_written(&ov, i, w, sizeof w);
                        if (it->secret) say("cmdline.txt will have: ", w[0] ? "wifi.psk=" PMASK " (the new password, hidden)" : "no WiFi password");
                        else say("cmdline.txt will have: ", w[0] ? w : "(the default: no option)");
                    }
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
                    psk_unmask(strget(G_CMD), cmd_base, sizeof cmd_base);
                    opt_init(&ov, cmd_base);
                    opts_show();
                    say("cmdline.txt taken as typed; the Settings page follows it (Save to write it)", 0);
                }
            }
        }
        bt_tick();
        if (page == 3) textset(G_TNOTE, changed() ? "Changes not saved yet: Save writes them (the old files are kept as .bak)" : "As on the card");
    }
end:
    if (win) {
        if (page >= 0 && glist_page[page]) RemoveGList(win, glist_page[page], -1);
        CloseWindow(win);
    }
    if (glist_top) FreeGadgets(glist_top);
    for (int i = 0; i < 5; i++) if (glist_page[i]) FreeGadgets(glist_page[i]);
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
