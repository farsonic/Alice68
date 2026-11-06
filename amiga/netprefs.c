/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-Network - the settings window for Alice68's network card: the
 * kernel's WiFi (alice68net.device unit 0) and Ethernet (unit 1).
 *
 * The status lines are read from the network board itself (alice_net.h)
 * twice a second; commands go to alice68net.device without waiting, so the
 * window never blocks. "Join" also saves the network in ENV: and
 * ENVARC:alice68wifi.prefs, which "Alice68-WiFi RECONNECT" in
 * S:User-Startup uses at boot. */
#include "wifi/wifios.h"
#include "wifi/wifi_api.h"
#include "../third_party/Emu68/src/alice/alice_net.h"
#include "../third_party/Emu68/src/alice/wifi/fmac.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }
static int memcmp(const void *a, const void *b, unsigned long n) { const UBYTE *x = a, *y = b; while (n--) if (*x++ != *y++) return 1; return 0; }

#define VERSTR "Alice68-Network 1.1"
ULONG lvo_y(APTR base, LONG off, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG a0, ULONG a1, ULONG a2, ULONG a3);
#define LY(b, o, d0, d1, d2, d3, a0, a1, a2, a3) lvo_y((APTR)(b), (o), (ULONG)(d0), (ULONG)(d1), (ULONG)(d2), (ULONG)(d3), (ULONG)(a0), (ULONG)(a1), (ULONG)(a2), (ULONG)(a3))

static APTR dos, intui, gt, gfx;
static ULONG out;
static void put(const char *s) { LONG n = 0; while (s[n]) n++; if (out) Write(dos, out, s, n); }   /* none from Workbench */

/* intuition, gadtools, exec */
static APTR OpenWindowTagList(const struct TagItem *t)  { return (APTR)LY(intui, -606, 0, 0, 0, 0, 0, t, 0, 0); }
static void CloseWindow(APTR w)                         { LY(intui, -72, 0, 0, 0, 0, w, 0, 0, 0); }
static APTR LockPubScreen(const char *n)                { return (APTR)LY(intui, -510, 0, 0, 0, 0, n, 0, 0, 0); }
static void UnlockPubScreen(APTR s)                     { LY(intui, -516, 0, 0, 0, 0, 0, s, 0, 0); }
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
static void Move_(APTR rp, LONG x, LONG y)              { LY(gfx, -240, (ULONG)x, (ULONG)y, 0, 0, 0, rp, 0, 0); }
static void Text_(APTR rp, const char *s, ULONG n)      { LY(gfx, -60, n, 0, 0, 0, s, rp, 0, 0); }
static void SetAPen(APTR rp, ULONG p)                   { LY(gfx, -342, p, 0, 0, 0, 0, rp, 0, 0); }

#define WA_(n)          (TAG_USER + 99 + (n))
#define GT_(n)          (TAG_USER + 0x80000 + (n))
#define GTLV_Labels     GT_(6)
#define GTTX_Text       GT_(11)
#define GTST_String     GT_(45)
#define GTST_MaxChars   GT_(46)
#define GT_VisualInfo   GT_(52)
#define GTLV_ShowSelected GT_(53)
#define GTLV_Selected   GT_(54)
#define GTTX_Border     GT_(57)
#define GTBB_Recessed   GT_(51)
#define GA_Disabled     (TAG_USER + 0x30000 + 14)
#define BUTTON_KIND     1
#define LISTVIEW_KIND   4
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
#define IDCMP_INTUITICKS    0x400000u

struct TextAttr_ { const char *name; UWORD ysize; UBYTE style, flags; };
struct NewGadget_ { WORD left, top, width, height; const char *text; const struct TextAttr_ *ta; UWORD id; ULONG flags; APTR vi; APTR user; };
ACHECK(sizeof(struct NewGadget_) == 30);

enum { G_WSTAT = 1, G_LIST, G_SCAN, G_DISC, G_WON, G_WOFF, G_SSID, G_PASS, G_CC, G_JOIN, G_ESTAT, G_EON, G_EOFF, G_N };
static APTR gad[G_N];
static UBYTE *win;
static APTR vi;

/* ---- the board: status lines -------------------------------------------- */
static volatile UBYTE *board;
static void find_board(void)
{
    APTR eb = OpenLibrary("expansion.library", 36);
    if (!eb) return;
    struct ConfigDev *cd = FindConfigDev(eb, 0, AN_MANUFACTURER, AN_PRODUCT);
    CloseLibrary(eb);
    if (cd && ((volatile struct an_hdr *)cd->cd_BoardAddr)->magic == AN_MAGIC) board = cd->cd_BoardAddr;
}
struct sb { char *p; unsigned n, max; };
static void sc(struct sb *b, char c) { if (b->n + 1 < b->max) b->p[b->n++] = c; b->p[b->n] = 0; }
static void ss(struct sb *b, const volatile char *s) { while (s && *s) sc(b, *s++); }
static void sd(struct sb *b, LONG v) { char t[12]; int n = 0; ULONG u = v < 0 ? (ULONG)-v : (ULONG)v; if (v < 0) sc(b, '-'); do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u); while (n) sc(b, t[--n]); }
static const char *secname(ULONG s)
{
    return s & SEC_WPA3 ? (s & SEC_WPA2 ? "WPA2/WPA3" : "WPA3") : s & SEC_WPA2 ? "WPA2" : s & SEC_WPA ? "WPA" :
           s & SEC_8021X ? "Enterprise" : s & SEC_WEP ? "WEP" : "open";
}
static const char *const stname[] = { "off", "starting", "ready", "scanning", "joining", "connected", "retrying", "FAILED", "no firmware" };
static void unit_line(int u, char *buf, unsigned max)
{
    struct sb b = { buf, 0, max };
    buf[0] = 0;
    if (!board) { ss(&b, "no Alice68 network board: not an Alice68 kernel with the network"); return; }
    volatile struct an_unit *x = (volatile struct an_unit *)(board + AN_OFF_UNIT(u));
    if (x->kind == AN_KIND_NONE) { ss(&b, u ? "not in use (eth=off in cmdline.txt)" : "not in use"); return; }
    ULONG st = x->state;
    ss(&b, st < 9 ? stname[st] : "?");
    if (u == 0 && st == AN_ST_CONNECTED) {
        ss(&b, ": "); ss(&b, x->ssid); ss(&b, ", ch "); sd(&b, (LONG)x->channel); ss(&b, ", "); sd(&b, x->rssi);
        ss(&b, " dBm, "); sd(&b, (LONG)x->speed_mbps); ss(&b, " Mbit/s, "); ss(&b, secname(x->security));
    } else if (u == 1 && x->link) {
        ss(&b, ": "); sd(&b, (LONG)x->speed_mbps); ss(&b, " Mbit/s");
    } else if (x->msg[0]) { ss(&b, " - "); ss(&b, x->msg); }
}

/* ---- the prefs file ------------------------------------------------------- */
static struct WifiJoin pj;
static void prefs_load(void)
{
    static char buf[2048];
    ULONG fh = DosOpen(dos, "ENV:alice68wifi.prefs", MODE_OLDFILE);
    if (!fh) fh = DosOpen(dos, "ENVARC:alice68wifi.prefs", MODE_OLDFILE);
    if (!fh) return;
    LONG n = DosRead(dos, fh, buf, sizeof buf - 1);
    DosClose(dos, fh);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *l = buf; *l; ) {
        char *e = l; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        while (*l == ' ' || *l == '\t') l++;
        char *d = 0; int max = 0;
        if (!memcmp(l, "SSID", 4)) { d = pj.wj_Ssid; max = 33; l += 4; }
        else if (!memcmp(l, "PASSWORD", 8)) { d = pj.wj_Pass; max = 65; l += 8; }
        else if (!memcmp(l, "COUNTRY", 7)) { d = pj.wj_Country; max = 3; l += 7; }
        if (d) {
            while (*l == ' ' || *l == '=') l++;
            int q = *l == '"', i = 0;
            if (q) l++;
            while (*l && i < max - 1 && (q ? *l != '"' : (*l != ' ' && *l != '\r'))) d[i++] = *l++;
            d[i] = 0;
        }
        *e = save;
        l = *e ? e + 1 : e;
    }
}
static int prefs_save(void)
{
    static char t[256];
    struct sb b = { t, 0, sizeof t };
    ss(&b, "SSID=\""); ss(&b, pj.wj_Ssid); ss(&b, "\"\nPASSWORD=\""); ss(&b, pj.wj_Pass); ss(&b, "\"\n");
    if (pj.wj_Country[0]) { ss(&b, "COUNTRY="); ss(&b, pj.wj_Country); ss(&b, "\n"); }
    int ok = 0;
    static const char *const files[] = { "ENV:alice68wifi.prefs", "ENVARC:alice68wifi.prefs" };
    for (int f = 0; f < 2; f++) {
        ULONG fh = DosOpen(dos, files[f], 1006);
        if (!fh) continue;
        if (Write(dos, fh, t, (LONG)b.n) == (LONG)b.n) ok++;
        DosClose(dos, fh);
    }
    return ok;
}

/* ---- the device: requests that do not block the window ------------------- */
static struct MsgPort *ioport;
static struct IOStdReq *io_scan, *io_w, *io_e;      /* scan, other WiFi commands, Ethernet */
static int open_w, open_e, busy_scan, busy_w, busy_e;
static struct WifiScanEntry scan[AN_SCAN_MAX];
static struct WifiJoin jn;
static char note[120];                               /* the last thing a button did */
static void send(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len, int *busy)
{
    if (*busy) return;
    io->io.io_Command = cmd; io->io_Data = data; io->io_Length = len;
    SendIO(io);
    *busy = 1;
}

/* the networks list */
struct Node_ { struct Node_ *succ, *pred; UBYTE type; BYTE pri; char *name; };
static struct { struct Node_ *head, *tail, *tailpred; UBYTE type, pad; } lvlist;
static struct Node_ nodes[AN_SCAN_MAX];
static char names[AN_SCAN_MAX][72];
static int nscan;
static void list_fill(int n)
{
    struct TagItem off[] = { { GTLV_Labels, ~0u }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(gad[G_LIST], win, off);
    lvlist.head = (struct Node_ *)&lvlist.tail; lvlist.tail = 0; lvlist.tailpred = (struct Node_ *)&lvlist.head;
    nscan = n;
    for (int i = 0; i < n; i++) {
        struct sb b = { names[i], 0, sizeof names[i] };
        sd(&b, scan[i].we_Rssi); ss(&b, " dBm  ch "); sd(&b, scan[i].we_Channel);
        while (b.n < 16) sc(&b, ' ');
        ss(&b, secname(scan[i].we_Security));
        while (b.n < 28) sc(&b, ' ');
        ss(&b, scan[i].we_Ssid);
        struct Node_ *nd = &nodes[i];
        nd->name = names[i];
        nd->succ = (struct Node_ *)&lvlist.tail; nd->pred = lvlist.tailpred;
        lvlist.tailpred->succ = nd; lvlist.tailpred = nd;
    }
    struct TagItem on[] = { { GTLV_Labels, (ULONG)&lvlist }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(gad[G_LIST], win, on);
}
static const char *strget(int g) { UBYTE *gg = gad[g]; return *(const char **)*(APTR *)(gg + 34); }    /* Gadget.SpecialInfo -> StringInfo.Buffer */
static void strset(int g, const char *s) { struct TagItem t[] = { { GTST_String, (ULONG)s }, { TAG_DONE, 0 } }; GT_SetGadgetAttrsA(gad[g], win, t); }
static void textset(int g, const char *s) { struct TagItem t[] = { { GTTX_Text, (ULONG)s }, { TAG_DONE, 0 } }; GT_SetGadgetAttrsA(gad[g], win, t); }

static char wline[160], eline[160], wshown[160], eshown[160];
static void refresh_status(void)
{
    unit_line(0, wline, sizeof wline);
    if (note[0] && !busy_w) { int n = 0; while (wline[n]) n++; struct sb b = { wline, (unsigned)n, sizeof wline }; ss(&b, "   ["); ss(&b, note); ss(&b, "]"); }
    unit_line(1, eline, sizeof eline);
    int dw = 0, de = 0;
    for (int i = 0; i < 160; i++) { if (wline[i] != wshown[i]) dw = 1; if (eline[i] != eshown[i]) de = 1; if (!wline[i] && !wshown[i] && !eline[i] && !eshown[i]) break; }
    if (dw) { memcpy(wshown, wline, sizeof wline); textset(G_WSTAT, wshown); }
    if (de) { memcpy(eshown, eline, sizeof eline); textset(G_ESTAT, eshown); }
}

static void box_labels(UBYTE *w, int x0, int y0)
{
    APTR rp = *(APTR *)(w + 50);
    struct TagItem t[] = { { GT_VisualInfo, (ULONG)vi }, { GTBB_Recessed, 1 }, { TAG_DONE, 0 } };
    DrawBevelBoxA(rp, x0 + 4, y0 + 10, 600, 158, t);
    DrawBevelBoxA(rp, x0 + 4, y0 + 182, 600, 46, t);
    SetAPen(rp, 2);
    Move_(rp, x0 + 12, y0 + 7); Text_(rp, "WiFi (unit 0)", 13);
    Move_(rp, x0 + 12, y0 + 179); Text_(rp, "Ethernet (unit 1)", 17);
}

LONG netprefs_main(const char *args, ULONG arglen)
{
    (void)args; (void)arglen;
    /* started from Workbench: its message first, answered at the very end */
    UBYTE *me = FindTask(0);
    APTR wbmsg = 0;
    if (!*(ULONG *)(me + 172)) {                    /* pr_CLI */
        lvo(SYSBASE, -384, 0, 0, (ULONG)(me + 92), 0);   /* WaitPort(pr_MsgPort) */
        wbmsg = GetMsg(me + 92);
    }
    dos = OpenLibrary("dos.library", 37);
    if (!dos) { if (wbmsg) { Forbid(); ReplyMsg(wbmsg); } return 20; }
    out = Output(dos);
    intui = OpenLibrary("intuition.library", 37);
    gt = OpenLibrary("gadtools.library", 37);
    gfx = OpenLibrary("graphics.library", 37);
    LONG rc = 0;
    APTR scr = 0, glist = 0;
    struct MsgPort *tp = 0; struct timerequest *tr = 0; int timer_ok = 0, tout = 0;
    if (!intui || !gt || !gfx) { put(VERSTR ": needs Kickstart 3 (gadtools.library 37)\n"); rc = 20; goto end; }
    find_board();
    ioport = CreateMsgPort();
    io_scan = ioport ? CreateIORequest(ioport, sizeof *io_scan) : 0;
    io_w = ioport ? CreateIORequest(ioport, sizeof *io_w) : 0;
    io_e = ioport ? CreateIORequest(ioport, sizeof *io_e) : 0;
    if (!io_scan || !io_w || !io_e) { rc = 20; goto end; }
    open_w = !OpenDevice("alice68net.device", 0, io_w, 0);
    if (open_w) { *io_scan = *io_w; }
    open_e = !OpenDevice("alice68net.device", 1, io_e, 0);
    tp = CreateMsgPort();
    tr = tp ? CreateIORequest(tp, sizeof *tr) : 0;
    timer_ok = tr && !OpenDevice("timer.device", 1, tr, 0);
    prefs_load();

    scr = LockPubScreen(0);
    if (!scr) { rc = 20; goto end; }
    vi = GetVisualInfoA(scr, 0);
    UBYTE *s = scr;
    int x0 = s[36], y0 = s[35] + (*(UWORD *)(*(UBYTE **)(s + 40) + 4)) + 1 + 4;    /* WBorLeft; WBorTop + font + 1 */
    static const struct TextAttr_ topaz = { "topaz.font", 8, 0, 0 };
    struct NewGadget_ ng;
    APTR g = CreateContext(&glist);
    #define NG(id_, x, y, w, h, txt, fl) (ng.left = (WORD)(x0 + (x)), ng.top = (WORD)(y0 + (y)), ng.width = (w), ng.height = (h), \
        ng.text = (txt), ng.ta = &topaz, ng.id = (id_), ng.flags = (fl), ng.vi = vi, ng.user = 0, &ng)
    struct TagItem txt[] = { { GTTX_Border, 1 }, { TAG_DONE, 0 } };
    gad[G_WSTAT] = g = CreateGadgetA(TEXT_KIND, g, NG(G_WSTAT, 12, 16, 584, 12, 0, 0), txt);
    struct TagItem lvt[] = { { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };
    gad[G_LIST] = g = CreateGadgetA(LISTVIEW_KIND, g, NG(G_LIST, 12, 32, 440, 92, 0, 0), lvt);
    gad[G_SCAN] = g = CreateGadgetA(BUTTON_KIND, g, NG(G_SCAN, 460, 32, 136, 14, "Scan", PLACETEXT_IN), 0);
    gad[G_DISC] = g = CreateGadgetA(BUTTON_KIND, g, NG(G_DISC, 460, 50, 136, 14, "Disconnect", PLACETEXT_IN), 0);
    gad[G_WON]  = g = CreateGadgetA(BUTTON_KIND, g, NG(G_WON, 460, 74, 136, 14, "WiFi on", PLACETEXT_IN), 0);
    gad[G_WOFF] = g = CreateGadgetA(BUTTON_KIND, g, NG(G_WOFF, 460, 92, 136, 14, "WiFi off", PLACETEXT_IN), 0);
    struct TagItem st1[] = { { GTST_String, (ULONG)pj.wj_Ssid }, { GTST_MaxChars, 32 }, { TAG_DONE, 0 } };
    gad[G_SSID] = g = CreateGadgetA(STRING_KIND, g, NG(G_SSID, 80, 130, 200, 14, "Network", PLACETEXT_LEFT), st1);
    struct TagItem st2[] = { { GTST_String, (ULONG)pj.wj_Pass }, { GTST_MaxChars, 64 }, { TAG_DONE, 0 } };
    gad[G_PASS] = g = CreateGadgetA(STRING_KIND, g, NG(G_PASS, 364, 130, 124, 14, "Password", PLACETEXT_LEFT), st2);
    struct TagItem st3[] = { { GTST_String, (ULONG)pj.wj_Country }, { GTST_MaxChars, 2 }, { TAG_DONE, 0 } };
    gad[G_CC]   = g = CreateGadgetA(STRING_KIND, g, NG(G_CC, 564, 130, 32, 14, "Country", PLACETEXT_LEFT), st3);
    gad[G_JOIN] = g = CreateGadgetA(BUTTON_KIND, g, NG(G_JOIN, 12, 150, 200, 14, "Join and save", PLACETEXT_IN), 0);
    gad[G_ESTAT] = g = CreateGadgetA(TEXT_KIND, g, NG(G_ESTAT, 12, 188, 584, 12, 0, 0), txt);
    gad[G_EON]  = g = CreateGadgetA(BUTTON_KIND, g, NG(G_EON, 12, 206, 200, 14, "Ethernet on", PLACETEXT_IN), 0);
    gad[G_EOFF] = g = CreateGadgetA(BUTTON_KIND, g, NG(G_EOFF, 220, 206, 200, 14, "Ethernet off", PLACETEXT_IN), 0);
    if (!g) { put(VERSTR ": the gadgets could not be made\n"); rc = 20; goto end; }
    struct TagItem wt[] = {
        { WA_(19), 616 }, { WA_(20), 236 }, { WA_(9), (ULONG)glist }, { WA_(11), (ULONG)VERSTR },
        { WA_(31), 1 }, { WA_(32), 1 }, { WA_(33), 1 }, { WA_(38), 1 }, { WA_(42), 1 }, { WA_(22), (ULONG)scr },
        { WA_(7), IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE |
                  IDCMP_MOUSEBUTTONS | IDCMP_INTUITICKS }, { TAG_DONE, 0 } };
    win = OpenWindowTagList(wt);
    UnlockPubScreen(scr); scr = 0;
    if (!win) { put(VERSTR ": the window would not open\n"); rc = 20; goto end; }
    GT_RefreshWindow(win);
    box_labels(win, x0, y0 - 4);
    if (!board) put(VERSTR ": no Alice68 network board\n");
    if (!open_w) { textset(G_WSTAT, "alice68net.device unit 0 would not open: is it in DEVS:?"); }
    refresh_status();

    ULONG wmask = 1u << ((struct MsgPort *)*(APTR *)(win + 86))->mp_SigBit, imask = 1u << ioport->mp_SigBit;
    ULONG tmask = timer_ok ? 1u << tp->mp_SigBit : 0;
    int quit = 0;
    while (!quit) {
        if (timer_ok && !tout) { tr->io.io_Command = TR_ADDREQUEST; tr->tv_secs = 0; tr->tv_micro = 500000; SendIO(tr); tout = 1; }
        ULONG got = Wait(wmask | imask | tmask | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C) quit = 1;
        if (tout && CheckIO(tr)) { WaitIO(tr); tout = 0; }
        /* finished requests */
        struct IOStdReq *io;
        while ((io = GetMsg(ioport))) {
            if (io == io_scan) {
                busy_scan = 0;
                if (io->io.io_Error) { note[0] = 0; struct sb b = { note, 0, sizeof note }; ss(&b, "scan failed (is the WiFi on?)"); }
                else { list_fill((int)io->io_Actual); note[0] = 0; }
            } else if (io == io_w) {
                busy_w = 0;
                if (io->io.io_Error) { struct sb b = { note, 0, sizeof note }; note[0] = 0; ss(&b, "the WiFi did not take it"); }
            } else if (io == io_e) busy_e = 0;
        }
        UBYTE *m;
        while ((m = GT_GetIMsg(*(APTR *)(win + 86)))) {
            ULONG cls = *(ULONG *)(m + 20); UWORD code = *(UWORD *)(m + 24); UBYTE *ia = *(UBYTE **)(m + 28);
            GT_ReplyIMsg(m);
            if (cls == IDCMP_CLOSEWINDOW) quit = 1;
            else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); box_labels(win, x0, y0 - 4); GT_EndRefresh(win, 1); }
            else if (cls == IDCMP_GADGETUP) {
                UWORD id = *(UWORD *)(ia + 38);
                struct sb b = { note, 0, sizeof note };
                if (id == G_LIST && code < nscan) strset(G_SSID, scan[code].we_Ssid);
                else if (id == G_SCAN && open_w && !busy_scan) {
                    send(io_scan, WIFI_CMD_SCAN, scan, sizeof scan, &busy_scan);
                    note[0] = 0; ss(&b, "scanning...");
                } else if (id == G_JOIN && open_w) {
                    memset(&jn, 0, sizeof jn);
                    const char *a = strget(G_SSID), *p = strget(G_PASS), *c = strget(G_CC);
                    for (int i = 0; a[i] && i < 32; i++) jn.wj_Ssid[i] = a[i];
                    for (int i = 0; p[i] && i < 64; i++) jn.wj_Pass[i] = p[i];
                    if (c[0] && c[1]) { jn.wj_Country[0] = (char)(c[0] & ~0x20); jn.wj_Country[1] = (char)(c[1] & ~0x20); }
                    note[0] = 0;
                    if (!jn.wj_Ssid[0]) ss(&b, "no network name");
                    else if (busy_w) ss(&b, "busy - try again in a moment");
                    else {
                        pj = jn;
                        send(io_w, WIFI_CMD_JOIN, &jn, sizeof jn, &busy_w);
                        ss(&b, prefs_save() ? "joining; saved in ENVARC:alice68wifi.prefs" : "joining; the prefs could not be saved");
                    }
                } else if ((id == G_DISC || id == G_WON || id == G_WOFF) && busy_w) { note[0] = 0; ss(&b, "busy - try again in a moment"); }
                else if (id == G_DISC && open_w) { send(io_w, WIFI_CMD_DISCONNECT, 0, 0, &busy_w); note[0] = 0; ss(&b, "disconnecting"); }
                else if (id == G_WON && open_w) { send(io_w, WIFI_CMD_START, 0, 0, &busy_w); note[0] = 0; ss(&b, "starting the WiFi"); }
                else if (id == G_WOFF && open_w) { send(io_w, WIFI_CMD_OFF, 0, 0, &busy_w); note[0] = 0; ss(&b, "switching the WiFi off"); }
                else if (id == G_EON && open_e) send(io_e, WIFI_CMD_START, 0, 0, &busy_e);
                else if (id == G_EOFF && open_e) send(io_e, WIFI_CMD_OFF, 0, 0, &busy_e);
            }
        }
        refresh_status();
    }
end:
    /* requests still out are taken back before the port goes */
    if (busy_scan) { AbortIO(io_scan); WaitIO(io_scan); }
    if (busy_w) { AbortIO(io_w); WaitIO(io_w); }
    if (busy_e) { AbortIO(io_e); WaitIO(io_e); }
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(scr);
    if (timer_ok) { if (tout) { AbortIO(tr); WaitIO(tr); } CloseDevice(tr); }
    if (tr) DeleteIORequest(tr);
    if (tp) DeleteMsgPort(tp);
    if (open_w) CloseDevice(io_w);
    if (open_e) CloseDevice(io_e);
    if (io_scan) DeleteIORequest(io_scan);
    if (io_w) DeleteIORequest(io_w);
    if (io_e) DeleteIORequest(io_e);
    if (ioport) DeleteMsgPort(ioport);
    if (gfx) CloseLibrary(gfx);
    if (gt) CloseLibrary(gt);
    if (intui) CloseLibrary(intui);
    CloseLibrary(dos);
    if (wbmsg) { Forbid(); ReplyMsg(wbmsg); }        /* Workbench unloads us after this */
    return rc;
}
