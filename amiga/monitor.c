/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-Monitor - a window that keeps showing what the machine is doing
 * while something hangs.
 *
 *   Alice68-Monitor          the window (run it first: "run Alice68-Monitor")
 *   Alice68-Monitor DUMP     print everything to the Shell - also after a hang
 *                            and a reset (Ctrl-Amiga-Amiga): the board keeps it
 *   Alice68-Monitor TIMERS   does each timer.device unit still answer? (run it while
 *                            something hangs: the Pi's timer bounds the wait)
 *
 * It shows:
 *   - what alice68wifi.device is doing now and for how long (the driver puts
 *     that on the USB pass-through board before every call that can block),
 *     and whether its heartbeat still moves;
 *   - every task: state, priority, the signals it waits for and has got, and
 *     a backtrace guessed from its stack (return addresses after JSR/BSR,
 *     named after the ROM module or "wifi+offset" in the WiFi driver);
 *   - the logs: the Alice68 kernel's (all of it), alice68wifi.device's and
 *     alice68-usb.device's rings on the board, as they grow.
 *
 * It runs at priority 21, above input.device, and asks nothing of DOS or any
 * filesystem while it runs: if the mouse still moves, it still updates. The
 * last task table goes onto the board too, for DUMP after a reset. */
#include "wifi/wifios.h"
#include "../third_party/Emu68/src/frf_usb/xhci/alice68_usbpt.h"
#include "../third_party/Emu68/src/alice/alice_net.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

#define MON_VERSION "Alice68-Monitor 1.2"

/* ---- system calls not in wifios.h ------------------------------------------ */
static inline APTR GetMsg_(APTR p)                     { return (APTR)lvo(SYSBASE, -372, 0, 0, (ULONG)p, 0); }
static inline void SetTaskPri(APTR t, LONG pri)       { lvo(SYSBASE, -300, (ULONG)pri, 0, 0, (ULONG)t); }
static APTR GfxIntui[2];                                  /* graphics, intuition */
#define GFX  GfxIntui[0]
#define INTUI GfxIntui[1]
static inline APTR OpenWindowTagList(const struct TagItem *t) { return (APTR)lvo(INTUI, -606, 0, 0, 0, (ULONG)t); }
static inline void CloseWindow(APTR w)                 { lvo(INTUI, -72, 0, 0, (ULONG)w, 0); }
static inline APTR LockPubScreen(const char *n)        { return (APTR)lvo(INTUI, -510, 0, 0, (ULONG)n, 0); }
static inline void UnlockPubScreen(APTR s)             { lvo(INTUI, -516, 0, 0, 0, (ULONG)s); }
static inline void Move_(APTR rp, LONG x, LONG y)      { lvo(GFX, -240, (ULONG)x, (ULONG)y, 0, (ULONG)rp); }
static inline void Text_(APTR rp, const char *s, ULONG n) { lvo(GFX, -60, n, 0, (ULONG)s, (ULONG)rp); }
static inline void SetAPen(APTR rp, ULONG p)           { lvo(GFX, -342, p, 0, 0, (ULONG)rp); }
static inline void SetBPen(APTR rp, ULONG p)           { lvo(GFX, -348, p, 0, 0, (ULONG)rp); }
static inline void SetDrMd(APTR rp, ULONG m)           { lvo(GFX, -354, m, 0, 0, (ULONG)rp); }

#define WA_(n)        (TAG_USER + 99 + (n))
#define WA_Left       WA_(1)
#define WA_Top        WA_(2)
#define WA_Width      WA_(3)
#define WA_Height     WA_(4)
#define WA_IDCMP      WA_(7)
#define WA_Title      WA_(11)
#define WA_DragBar    WA_(31)
#define WA_DepthGadget WA_(32)
#define WA_CloseGadget WA_(33)
#define WA_NoCareRefresh WA_(36)
#define WA_Activate   WA_(38)
#define WA_RMBTrap    WA_(39)
#define WA_SmartRefresh WA_(42)
#define IDCMP_CLOSEWINDOW 0x200u
#define IDCMP_VANILLAKEY  0x200000u

/* ---- small things ---------------------------------------------------------------- */
static ULONG usecs(void)
{
    ULONG v = *(volatile ULONG *)0xF2003004u;           /* the Pi's system timer, 1 MHz, little-endian */
    return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
}
struct sb { char *p; unsigned n, max; };
static void sc(struct sb *b, char c) { if (b->n + 1 < b->max) b->p[b->n++] = c; b->p[b->n] = 0; }
static void ss(struct sb *b, const char *s) { while (s && *s) sc(b, *s++); }
static void shex(struct sb *b, ULONG v, int d) { for (int i = d - 1; i >= 0; i--) sc(b, "0123456789abcdef"[(v >> (i * 4)) & 15]); }
static void sdec(struct sb *b, ULONG v) { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) sc(b, t[--n]); }
static void spad(struct sb *b, unsigned col) { while (b->n < col) sc(b, ' '); }
static void ssec(struct sb *b, ULONG us)    /* "12.3 s" */
{
    sdec(b, us / 1000000); sc(b, '.'); sc(b, (char)('0' + us / 100000 % 10)); ss(b, " s");
}
static int sieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* ---- the board ----------------------------------------------------------------- */
static volatile struct pt_hdr *board;
static volatile struct pt_wstat *wst;
static volatile UBYTE *netb;                            /* the kernel's network board (alice_net.h) */
static void find_board(void)
{
    APTR eb = OpenLibrary("expansion.library", 36);
    if (!eb) return;
    struct ConfigDev *nc = FindConfigDev(eb, 0, AN_MANUFACTURER, AN_PRODUCT);
    if (nc && ((volatile struct an_hdr *)nc->cd_BoardAddr)->magic == AN_MAGIC) netb = nc->cd_BoardAddr;
    struct ConfigDev *cd = FindConfigDev(eb, 0, PT_MANUFACTURER, PT_PRODUCT);
    CloseLibrary(eb);
    if (!cd) return;
    volatile struct pt_hdr *b = cd->cd_BoardAddr;
    if (b->magic != PT_MAGIC) return;
    board = b;
    volatile struct pt_wstat *w = (volatile struct pt_wstat *)((volatile char *)b + PT_OFF_WSTAT);
    if (w->magic == PT_WSTAT_MAGIC) wst = w;
}

/* ---- memory: what may be read, and what code lies where -------------------------- */
#define MAXRANGE 24
static ULONG rlo[MAXRANGE], rhi[MAXRANGE];
static int nrange;
static ULONG wifi_lo, wifi_hi;                          /* alice68wifi.device's code */
static void add_range(ULONG lo, ULONG hi) { if (nrange < MAXRANGE && hi > lo) { rlo[nrange] = lo; rhi[nrange] = hi; nrange++; } }
static void scan_memory(void)
{
    UBYTE *sys = SYSBASE;
    nrange = 0;
    Forbid();
    for (struct Node *n = ((struct List *)(sys + 322))->lh_Head; n->ln_Succ; n = n->ln_Succ)
        add_range(*(ULONG *)((UBYTE *)n + 20), *(ULONG *)((UBYTE *)n + 24));
    UBYTE *dev = FindName(sys + EXEC_DEVICELIST, "alice68wifi.device");
    if (dev) {
        ULONG seg = *(ULONG *)(dev + 36) << 2;          /* struct WifiBase: lib, pad0, seglist */
        if (seg) { wifi_lo = seg + 4; wifi_hi = seg - 4 + *(ULONG *)(seg - 4); }
    }
    Permit();
    add_range(0x00f80000u, 0x01000000u);                /* Kickstart */
    add_range(0x00e00000u, 0x00e80000u);
    add_range(0x00f00000u, 0x00f80000u);
    APTR eb = OpenLibrary("expansion.library", 36);
    if (eb) {                                           /* Emu68's ROM board and the others */
        for (struct ConfigDev *c = FindConfigDev(eb, 0, -1, -1); c; c = FindConfigDev(eb, c, -1, -1))
            if (!(c->cd_Rom[0] & 0x20)) add_range((ULONG)c->cd_BoardAddr, (ULONG)c->cd_BoardAddr + c->cd_BoardSize);
        CloseLibrary(eb);
    }
}
static int readable(ULONG a, ULONG n)
{
    for (int i = 0; i < nrange; i++) if (a >= rlo[i] && a + n <= rhi[i]) return 1;
    return 0;
}
/* a return address: even, readable, and right after a JSR or BSR */
static int retaddr(ULONG a)
{
    if ((a & 1) || a < 0x400 || !readable(a - 6, 6)) return 0;
    UWORD w2 = *(UWORD *)(a - 2), w4 = *(UWORD *)(a - 4), w6 = *(UWORD *)(a - 6);
    if ((w2 & 0xfff8) == 0x4e90) return 1;                          /* jsr (An) */
    if ((w2 & 0xff00) == 0x6100 && (w2 & 0xff) != 0 && (w2 & 0xff) != 0xff) return 1;   /* bsr.b */
    if ((w4 & 0xfff8) == 0x4ea8 || w4 == 0x4eba || w4 == 0x6100) return 1;   /* jsr d16(An), jsr d16(pc), bsr.w */
    if ((w4 & 0xfff8) == 0x4eb0 || w4 == 0x4ebb) return 1;           /* jsr d8(An,Xn), d8(pc,Xn) */
    if (w6 == 0x4eb9 || w6 == 0x61ff) return 1;                     /* jsr abs.l, bsr.l */
    return 0;
}
/* "wifi+1a2c", "exec.library+3f0", or "$00f81234" */
static void sname(struct sb *b, ULONG a)
{
    if (a >= wifi_lo && a < wifi_hi) { ss(b, "wifi+"); shex(b, a - wifi_lo, 4); return; }
    UBYTE *sys = SYSBASE;
    ULONG *rm = *(ULONG **)(sys + 300), best = 0;
    for (int guard = 0; rm && guard < 2000; guard++) {
        ULONG r = *rm;
        if (!r) break;
        if (r & 0x80000000u) { rm = (ULONG *)(r & 0x7fffffffu); continue; }
        rm++;
        if (!readable(r, 26)) continue;
        ULONG end = *(ULONG *)(r + 6);
        if (r <= a && a < end && r > best) best = r;
    }
    if (best) {
        const char *nm = *(const char **)(best + 14);
        if (nm && readable((ULONG)nm, 1)) { for (int i = 0; nm[i] && i < 20; i++) sc(b, nm[i]); }
        else ss(b, "module");
        sc(b, '+'); shex(b, a - best, 4);
        return;
    }
    sc(b, '$'); shex(b, a, 8);
}

/* ---- tasks --------------------------------------------------------------------- */
#define MAXT 64
#define MAXBT 6
struct tinfo {
    char name[24];
    BYTE pri; UBYTE state, type, wifi;
    ULONG wait, recvd, sp;
    ULONG bt[MAXBT]; int nbt;
};
static struct tinfo tasks[MAXT];
static int ntasks;
static UBYTE id_nest, td_nest;

static void task_name(struct tinfo *t, UBYTE *tk)
{
    struct sb b = { t->name, 0, sizeof t->name };
    const char *n = ((struct Node *)tk)->ln_Name;
    if (n && readable((ULONG)n, 1)) for (int i = 0; n[i] && i < 22; i++) sc(&b, n[i]);
    if (tk[8] == 13) {                                  /* a process: a Shell's command name */
        ULONG cli = *(ULONG *)(tk + 172) << 2;
        if (cli && readable(cli, 64) && *(ULONG *)(cli + 60)) {
            ULONG bs = *(ULONG *)(cli + 16) << 2;
            if (bs && readable(bs, 1)) {
                UBYTE len = *(UBYTE *)bs;
                if (len && readable(bs, (ULONG)len + 1)) {
                    b.n = 0; b.p[0] = 0;
                    const char *c = (const char *)bs + 1;
                    int start = 0; for (int i = 0; i < len; i++) if (c[i] == ':' || c[i] == '/') start = i + 1;
                    for (int i = start; i < len; i++) sc(&b, c[i]);
                    ss(&b, " (Shell)");
                }
            }
        }
    }
}
static void take(UBYTE *tk, ULONG wtask)
{
    if (ntasks >= MAXT) return;
    struct tinfo *t = &tasks[ntasks++];
    task_name(t, tk);
    t->pri = (BYTE)tk[9]; t->type = tk[8]; t->state = tk[15];
    t->wait = *(ULONG *)(tk + 22); t->recvd = *(ULONG *)(tk + 26);
    t->wifi = (ULONG)tk == wtask;
    ULONG sp = *(ULONG *)(tk + 54), up = *(ULONG *)(tk + 62);
    t->sp = sp; t->nbt = 0;
    if (t->state == 2) return;                          /* running: that is us */
    ULONG end = sp + 2048;
    if (up > sp && up < end) end = up;
    for (ULONG a = sp; a + 4 <= end && t->nbt < MAXBT; a += 2) {
        if (!readable(a, 4)) break;
        ULONG v = *(ULONG *)a;
        if (retaddr(v)) t->bt[t->nbt++] = v;
    }
}
static void snapshot(void)
{
    UBYTE *sys = SYSBASE;
    ULONG wtask = wst ? wst->task : 0;
    ntasks = 0;
    Disable();
    id_nest = sys[294]; td_nest = sys[295];
    take(*(UBYTE **)(sys + 276), wtask);
    for (struct Node *n = ((struct List *)(sys + 406))->lh_Head; n->ln_Succ; n = n->ln_Succ) take((UBYTE *)n, wtask);
    for (struct Node *n = ((struct List *)(sys + 420))->lh_Head; n->ln_Succ; n = n->ln_Succ) take((UBYTE *)n, wtask);
    Enable();
    /* the WiFi driver's process first, then by priority */
    for (int i = 1; i < ntasks; i++)
        for (int j = i; j > 0; j--) {
            struct tinfo *a = &tasks[j - 1], *b = &tasks[j];
            int ka = a->wifi ? 1000 : a->pri, kb = b->wifi ? 1000 : b->pri;
            if (kb <= ka) break;
            struct tinfo t = *a; *a = *b; *b = t;
        }
}
static void task_line(struct sb *b, struct tinfo *t)
{
    static const char st[] = "?ARrWXD";                 /* added, Running, ready, Waiting, eXcept, removed */
    ss(b, t->wifi ? "*" : " ");
    ss(b, t->name); spad(b, 25);
    if (t->pri >= 0) sc(b, ' ');
    if (t->pri < 0) { sc(b, '-'); sdec(b, (ULONG)-t->pri); } else sdec(b, (ULONG)t->pri);
    spad(b, 30);
    sc(b, t->state < 7 ? st[t->state] : '?');
    ss(b, " w "); shex(b, t->wait, 8); ss(b, " g "); shex(b, t->recvd, 8);
    ss(b, "  ");
    for (int i = 0; i < t->nbt; i++) { if (i) sc(b, ' '); sname(b, t->bt[i]); }
}
static void wifi_line(struct sb *b)
{
    if (netb) {                                         /* the kernel's WiFi, on CPU1 */
        static const char *const sw[] = { "off", "starting", "ready", "scanning", "joining", "connected", "retrying", "FAILED", "no firmware" };
        volatile struct an_unit *u = (volatile struct an_unit *)(netb + AN_OFF_UNIT(0));
        ULONG st = u->state;
        ss(b, "WiFi (kernel): "); ss(b, st < 9 ? sw[st] : "?"); ss(b, " - ");
        char t[97];
        for (int i = 0; i < 96; i++) t[i] = u->msg[i];
        t[96] = 0; ss(b, t); ss(b, "  [");
        for (int i = 0; i < 96; i++) t[i] = u->where[i];
        t[96] = 0; ss(b, t); ss(b, "] polls "); sdec(b, u->polls); ss(b, " rx "); sdec(b, u->rx_pkts); ss(b, " tx "); sdec(b, u->tx_pkts);
        return;
    }
    if (!board) { ss(b, "no pass-through board (USB kernel needed for logs and driver state)"); return; }
    if (!wst) { ss(b, "alice68wifi.device: has not run since the Pi started"); return; }
    ULONG v = wst->ver;
    ss(b, "alice68wifi "); sdec(b, v >> 16); sc(b, '.'); sdec(b, v & 0xffff); ss(b, ": ");
    char wh[97]; for (int i = 0; i < 96; i++) wh[i] = wst->where[i]; wh[96] = 0;
    ss(b, wh);
    ss(b, "  (for "); ssec(b, usecs() - wst->where_us); ss(b, ", beat "); sdec(b, wst->beat); sc(b, ')');
}

/* ---- the logs ------------------------------------------------------------------------ */
#define NLINES 400
#define LW 120
static char lines[NLINES][LW];
static UBYTE lsrc[NLINES];
static unsigned lhead, lcount;
#define NSRC 4
static char part[NSRC][LW];
static unsigned partn[NSRC];
static ULONG seen[NSRC];
static const char srcch[NSRC] = { 'K', 'W', 'U', 'N' };
static void add_line(int src, const char *s, unsigned n)
{
    char *d = lines[lhead];
    unsigned k = 0;
    d[k++] = srcch[src]; d[k++] = ' ';
    for (unsigned i = 0; i < n && k < LW - 1; i++) d[k++] = s[i] == '\t' ? ' ' : s[i];
    d[k] = 0;
    lsrc[lhead] = (UBYTE)src;
    lhead = (lhead + 1) % NLINES;
    if (lcount < NLINES) lcount++;
}
static int ring_info(int src, volatile const char **r, ULONG *size, ULONG *wr)
{
    if (src == 3) {
        if (!netb) return 0;
        *r = (volatile const char *)netb + AN_OFF_LOG; *size = AN_LOG_BYTES; *wr = ((volatile struct an_hdr *)netb)->log_wr;
        return 1;
    }
    if (!board) return 0;
    if (src == 0) { *r = (volatile const char *)board + PT_OFF_KLOG; *size = PT_KLOG_BYTES; *wr = board->klog_wr; }
    else if (src == 1) { *r = (volatile const char *)board + PT_OFF_WLOG; *size = PT_WLOG_BYTES; *wr = board->wlog_wr; }
    else { *r = (volatile const char *)board + PT_OFF_ALOG; *size = PT_ALOG_BYTES; *wr = board->alog_wr; }
    return 1;
}
static int pull(int src)
{
    volatile const char *r; ULONG size, wr;
    if (!ring_info(src, &r, &size, &wr)) return 0;
    ULONG from = seen[src];
    if (wr - from > size) { from = wr - size; partn[src] = 0; }
    if (from == wr) return 0;
    for (ULONG i = from; i != wr; i++) {
        char c = r[i % size];
        if (c == '\r') continue;
        if (c == '\n' || partn[src] >= LW - 3) { add_line(src, part[src], partn[src]); partn[src] = 0; if (c == '\n') continue; }
        part[src][partn[src]++] = c;
    }
    seen[src] = wr;
    return 1;
}

/* ---- DUMP: everything to the Shell ------------------------------------------------- */
static APTR dos; static ULONG out;
static void put(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void dump_ring(const char *title, int src)
{
    volatile const char *r; ULONG size, wr;
    put("\n===== "); put(title); put(" =====\n");
    if (!ring_info(src, &r, &size, &wr)) { put("(no board)\n"); return; }
    char buf[256]; ULONG n = 0;
    for (ULONG i = wr > size ? wr - size : 0; i < wr; i++) {
        buf[n++] = r[i % size];
        if (n == sizeof buf) { Write(dos, out, buf, (LONG)n); n = 0; }
    }
    if (n) Write(dos, out, buf, (LONG)n);
}
static void put_snapshot_text(char *dst, unsigned max)
{
    struct sb b = { dst, 0, max };
    ss(&b, MON_VERSION " task table at "); ssec(&b, usecs()); ss(&b, " (Pi timer)\n");
    char l[200];
    struct sb lb = { l, 0, sizeof l };
    wifi_line(&lb); ss(&b, l); sc(&b, '\n');
    ss(&b, " task                   pri  state, waits for, got, backtrace\n");
    for (int i = 0; i < ntasks; i++) { lb.n = 0; l[0] = 0; task_line(&lb, &tasks[i]); ss(&b, l); sc(&b, '\n'); }
}
static void to_board(void)
{
    if (!board) return;
    static char t[PT_MSNAP_BYTES];
    put_snapshot_text(t, sizeof t);
    volatile char *d = (volatile char *)board + PT_OFF_MSNAP;
    unsigned i = 0;
    for (; i < sizeof t - 1 && t[i]; i++) d[i] = t[i];
    d[i] = 0;
    board->msnap_seq++;
}
static LONG dump(void)
{
    put(MON_VERSION " DUMP\n");
    if (!board && !netb) { put("no Alice68 boards: not an Alice68 kernel\n"); return 10; }
    char l[200]; struct sb lb = { l, 0, sizeof l };
    wifi_line(&lb); put(l); put("\n");
    if (netb) {                                         /* the kernel's Ethernet: how CPU1 serves the board */
        static const char *const se[] = { "off", "starting", "no link", "", "", "up", "", "FAILED", "" };
        volatile struct an_unit *e = (volatile struct an_unit *)(netb + AN_OFF_UNIT(1));
        char t[97];
        lb.n = 0; l[0] = 0;
        ss(&lb, "Ethernet (kernel): "); ss(&lb, e->state < 9 ? se[e->state] : "?");
        ss(&lb, ", rx "); sdec(&lb, e->rx_pkts); ss(&lb, " tx "); sdec(&lb, e->tx_pkts);
        ss(&lb, " dropped "); sdec(&lb, e->rx_drop); ss(&lb, "\n  ");
        for (int i = 0; i < 96; i++) t[i] = e->where[i];
        t[96] = 0; ss(&lb, t);
        put(l); put("\n");
    }
    if (wst) { put("  where changed "); lb.n = 0; sdec(&lb, wst->where_seq); put(l); put(" times\n"); }
    put("\n===== the monitor's last task table (from before a reset, if there was one) =====\n");
    if (board->msnap_seq) {
        volatile const char *s = (volatile const char *)board + PT_OFF_MSNAP;
        char buf[256]; unsigned n = 0;
        for (unsigned i = 0; i < PT_MSNAP_BYTES && s[i]; i++) { buf[n++] = s[i]; if (n == sizeof buf) { Write(dos, out, buf, (LONG)n); n = 0; } }
        if (n) Write(dos, out, buf, (LONG)n);
    } else put("(none: the monitor has not run since the Pi started)\n");
    put("\n===== the task table now =====\n");
    scan_memory(); snapshot();
    static char t[PT_MSNAP_BYTES];
    put_snapshot_text(t, sizeof t); put(t);
    dump_ring("kernel WiFi log (network board)", 3);
    dump_ring("alice68wifi.device log (old driver)", 1);
    dump_ring("alice68-usb.device log", 2);
    dump_ring("kernel log", 0);
    put("\n");
    return 0;
}

/* ---- the window ---------------------------------------------------------------------- */
static UBYTE *win;
static int cols, rows, lh, base, x0, y0;
static int filter;                       /* 0 all, 1 kernel, 2 wifi, 3 usb */
static int ntrows = 14;
static int paused;
static void row(int r, const char *s, int pen)
{
    if (r >= rows) return;
    APTR rp = *(APTR *)(win + 50);
    char b[256]; int n = 0;
    while (s[n] && n < cols && n < 255) { b[n] = s[n]; n++; }
    while (n < cols && n < 255) b[n++] = ' ';
    SetAPen(rp, (ULONG)pen); SetBPen(rp, 0);
    Move_(rp, x0, y0 + r * lh + base);
    Text_(rp, b, (ULONG)n);
}
static void draw(void)
{
    char l[256]; struct sb b = { l, 0, sizeof l };
    ss(&b, MON_VERSION "  space: pause  1 all  2 kernel  3 old WiFi  4 USB  5 network  t: tasks  q: quit");
    if (paused) ss(&b, "   PAUSED");
    row(0, l, 2);
    b.n = 0; l[0] = 0; wifi_line(&b); row(1, l, 1);
    int r = 2;
    if (ntrows) {
        b.n = 0; l[0] = 0;
        ss(&b, "-- "); sdec(&b, (ULONG)ntasks); ss(&b, " tasks: name, pri, state (W wait, r ready), waits for, got, backtrace; * = WiFi driver --");
        row(r++, l, 2);
        for (int i = 0; i < ntasks && i < ntrows; i++) { b.n = 0; l[0] = 0; task_line(&b, &tasks[i]); row(r++, l, tasks[i].wifi ? 3 : 1); }
    }
    b.n = 0; l[0] = 0;
    static const char *const fn[] = { "all logs (K kernel, N network, U USB)", "kernel log", "WiFi driver log", "USB driver log", "network log" };
    ss(&b, "-- "); ss(&b, fn[filter]); ss(&b, " --");
    row(r++, l, 2);
    int avail = rows - r;
    if (avail <= 0) return;
    /* the newest matching lines, oldest first */
    int pick[200], np = 0;
    for (unsigned k = 0; k < lcount && np < avail && np < 200; k++) {
        unsigned i = (lhead + NLINES - 1 - k) % NLINES;
        if (filter && lsrc[i] != filter - 1) continue;
        if (!filter && lsrc[i] == 3 && board) continue;    /* the kernel log has the network's lines already */
        pick[np++] = (int)i;
    }
    for (int k = 0; k < avail; k++) {
        int idx = np - avail + k;
        row(r + k, idx >= 0 ? lines[pick[np - 1 - idx]] : "", (lsrc[idx >= 0 ? pick[np - 1 - idx] : 0] & 1) ? 3 : 1);
    }
}

/* ---- TIMERS: does each timer.device unit still answer? --------------------------------- */
/* Run it while something hangs. Each unit gets a 200 ms request; the Pi's own timer decides
   when to stop waiting, so a dead unit cannot hang this too. MICROHZ and ECLOCK run on the
   CIAs, VBLANK on the vertical blank. */
static void put_ms(ULONG us) { char l[24]; struct sb b = { l, 0, sizeof l }; sdec(&b, us / 1000); ss(&b, " ms"); put(l); }
static void put_hex4(UWORD v)
{
    static const char hx[] = "0123456789abcdef";
    char l[5] = { hx[v >> 12], hx[(v >> 8) & 15], hx[(v >> 4) & 15], hx[v & 15], 0 };
    put(l);
}
static LONG timers(void)
{
    static const char *const name[3] = { "MICROHZ (CIA)", "VBLANK        ", "ECLOCK (CIA) " };
    LONG rc = 0;
    put(MON_VERSION " TIMERS\n");
    put("INTENAR $"); put_hex4(*(volatile UWORD *)0xDFF01C);
    put("  INTREQR $"); put_hex4(*(volatile UWORD *)0xDFF01E);
    put("  (bit 3 PORTS = CIA-A and the boards, 5 VERTB, 13 EXTER = CIA-B)\n");
    for (ULONG unit = 0; unit < 3; unit++) {
        struct MsgPort *tp = CreateMsgPort();
        struct timerequest *tr = tp ? CreateIORequest(tp, sizeof *tr) : 0;
        put("  "); put(name[unit]); put(": ");
        if (!tr || OpenDevice("timer.device", unit, tr, 0)) { put("could not open\n"); rc = 5; }
        else {
            tr->io.io_Command = TR_ADDREQUEST; tr->tv_secs = 0; tr->tv_micro = 200000;
            ULONG t0 = usecs();
            SendIO(tr);
            int done = 0;
            while (usecs() - t0 < 3000000u) if (CheckIO(tr)) { done = 1; break; }
            ULONG dt = usecs() - t0;
            if (done) { put("answered after "); put_ms(dt); put(" (asked for 200 ms)\n"); }
            else { put("NO ANSWER after "); put_ms(dt); put(" - this timer is stuck\n"); AbortIO(tr); rc = 5; }
            WaitIO(tr);
            CloseDevice(tr);
        }
        if (tr) DeleteIORequest(tr);
        if (tp) DeleteMsgPort(tp);
    }
    put("  INTREQR now $"); put_hex4(*(volatile UWORD *)0xDFF01E); put("\n");

    /* Does the E-clock only go forwards? Programs measure time with it (Riptide's ping:
       ReadEClock); one step back makes "until - now" huge and a 1 s wait endless. For 5 s,
       read it as fast as possible, next to the Pi's timer. */
    struct MsgPort *tp = CreateMsgPort();
    struct timerequest *tr = tp ? CreateIORequest(tp, sizeof *tr) : 0;
    if (tr && !OpenDevice("timer.device", 2, tr, 0)) {
        APTR tb = tr->io.io_Device;
        ULONG ev[2], f = lvo(tb, -60, 0, 0, (ULONG)ev, 0);      /* ReadEClock(a0): d0 = ticks/s */
        ULONG phi = ev[0], plo = ev[1], slo = ev[1], reads = 0, back = 0, jumps = 0, maxback = 0, maxfwd = 0;
        ULONG t0 = usecs(), pt = t0;
        put("  E-clock at "); { char l[16]; struct sb b = { l, 0, sizeof l }; sdec(&b, f); put(l); } put(" ticks/s, read for 5 s: ");
        while (usecs() - t0 < 5000000u) {
            ULONG c[2];
            lvo(tb, -60, 0, 0, (ULONG)c, 0);
            ULONG now = usecs();
            reads++;
            /* ticks since the last read, as a signed 64-bit difference held in 32 bits */
            ULONG dlo = c[1] - plo, dhi = c[0] - phi - (c[1] < plo);
            if ((LONG)dhi < 0) {                                  /* went backwards */
                ULONG nb = (ULONG)(-(LONG)dlo);
                back++;
                if (dhi != 0xffffffffu) nb = 0xffffffffu;
                if (nb > maxback) maxback = nb;
            } else {
                ULONG ticks = dhi ? 0xffffffffu : dlo;
                ULONG want = (now - pt) * (f / 1000u) / 1000u + f / 100u;   /* the Pi's view + 10 ms */
                if (ticks > want) { jumps++; if (ticks - want > maxfwd) maxfwd = ticks - want; }
            }
            phi = c[0]; plo = c[1]; pt = now;
        }
        char l[160]; struct sb b = { l, 0, sizeof l };
        sdec(&b, reads); ss(&b, " reads, "); sdec(&b, back); ss(&b, " went BACK (most ");
        sdec(&b, maxback); ss(&b, " ticks), "); sdec(&b, jumps); ss(&b, " jumped ahead (most ");
        sdec(&b, maxfwd); ss(&b, " ticks)\n");
        put(l);
        b.n = 0; l[0] = 0;
        ULONG el = usecs() - t0;
        ss(&b, "  it moved "); sdec(&b, plo - slo); ss(&b, " ticks in "); sdec(&b, el / 1000);
        ss(&b, " ms by the Pi's timer: "); sdec(&b, (ULONG)((plo - slo) / (el / 1000u ? el / 1000u : 1u)) * 1000u);
        ss(&b, " ticks/s\n");
        put(l);
        if (back || jumps) rc = 5;
        CloseDevice(tr);
    } else put("  E-clock: timer.device unit 2 would not open\n");
    if (tr) DeleteIORequest(tr);
    if (tp) DeleteMsgPort(tp);

    /* Are requests ever lost? Short waits, one after another, for 10 s, each 1..16 ms on
       MICROHZ (the CIA-A timer, as Riptide's sleeps), then VBLANK. A request not back within
       2 s of the Pi's timer is counted LOST and taken back with AbortIO. Run it while traffic
       flows (a speed test in another window): that is when level 2 is busiest. */
    for (ULONG unit = 0; unit < 2; unit++) {
        struct MsgPort *sp = CreateMsgPort();
        struct timerequest *sr = sp ? CreateIORequest(sp, sizeof *sr) : 0;
        if (!sr || OpenDevice("timer.device", unit ? 1 : 0, sr, 0)) { put("  stress: timer.device would not open\n"); rc = 5; }
        else {
            ULONG n = 0, lost = 0, late = 0, worst = 0, t0 = usecs(), seed = t0;
            put(unit ? "  VBLANK  waits for 10 s: " : "  MICROHZ waits for 10 s: ");
            while (usecs() - t0 < 10000000u) {
                seed = seed * 1103515245u + 12345u;
                ULONG want = 1000u + (seed >> 16) % 16000u;
                sr->io.io_Command = TR_ADDREQUEST; sr->tv_secs = 0; sr->tv_micro = want;
                ULONG a = usecs();
                SendIO(sr);
                int done = 0;
                while (usecs() - a < want + 2000000u) if (CheckIO(sr)) { done = 1; break; }
                ULONG took = usecs() - a;
                if (!done) { lost++; AbortIO(sr); }
                WaitIO(sr);
                n++;
                if (done && took > want + 40000u) late++;
                if (done && took - want > worst && took > want) worst = took - want;
            }
            char l[160]; struct sb b = { l, 0, sizeof l };
            sdec(&b, n); ss(&b, " waits, "); sdec(&b, lost); ss(&b, " LOST, "); sdec(&b, late);
            ss(&b, " more than 40 ms late, worst "); sdec(&b, worst / 1000); ss(&b, " ms late\n");
            put(l);
            if (lost) rc = 5;
            CloseDevice(sr);
        }
        if (sr) DeleteIORequest(sr);
        if (sp) DeleteMsgPort(sp);
    }
    return rc;
}

LONG monitor_main(const char *args, ULONG arglen)
{
    (void)arglen;
    dos = OpenLibrary("dos.library", 37);
    if (!dos) return 20;
    out = Output(dos);
    char w0[16]; int n = 0;
    while (*args == ' ' || *args == '\t') args++;
    while (*args && *args != ' ' && *args != '\n' && n < 15) w0[n++] = *args++;
    w0[n] = 0;
    find_board();
    if (sieq(w0, "DUMP")) { LONG rc = dump(); CloseLibrary(dos); return rc; }
    if (sieq(w0, "TIMERS")) { LONG rc = timers(); CloseLibrary(dos); return rc; }
    if (w0[0]) { put("Alice68-Monitor [DUMP|TIMERS]\n"); CloseLibrary(dos); return w0[0] == '?' ? 0 : 10; }

    GFX = OpenLibrary("graphics.library", 37);
    INTUI = OpenLibrary("intuition.library", 37);
    struct MsgPort *tp = CreateMsgPort();
    struct timerequest *tr = tp ? CreateIORequest(tp, sizeof *tr) : 0;
    int timer_ok = tr && !OpenDevice("timer.device", 1, tr, 0);      /* UNIT_VBLANK */
    APTR scr = INTUI ? LockPubScreen(0) : 0;
    if (!GFX || !INTUI || !timer_ok || !scr) { put("Alice68-Monitor: no graphics, intuition, timer or Workbench screen\n"); goto out; }
    WORD sw = *(WORD *)((UBYTE *)scr + 12), sh = *(WORD *)((UBYTE *)scr + 14);
    UBYTE bar = *((UBYTE *)scr + 30);                    /* BarHeight */
    struct TagItem t[] = {
        { WA_Left, 0 }, { WA_Top, (ULONG)bar + 1 }, { WA_Width, (ULONG)sw }, { WA_Height, (ULONG)(sh - bar - 1) },
        { WA_Title, (ULONG)MON_VERSION }, { WA_DragBar, 1 }, { WA_DepthGadget, 1 }, { WA_CloseGadget, 1 },
        { WA_SmartRefresh, 1 }, { WA_NoCareRefresh, 1 }, { WA_Activate, 1 }, { WA_RMBTrap, 1 },
        { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY }, { TAG_DONE, 0 } };
    win = OpenWindowTagList(t);
    UnlockPubScreen(scr);
    if (!win) { put("Alice68-Monitor: the window would not open\n"); goto out; }
    put(MON_VERSION ": window open, priority 21 (Alice68-Monitor DUMP prints it all, also after a reset)\n");
    APTR rp = *(APTR *)(win + 50);
    lh = *(UWORD *)((UBYTE *)rp + 58); base = *(UWORD *)((UBYTE *)rp + 62);
    int cw = *(UWORD *)((UBYTE *)rp + 60);
    if (lh < 6) lh = 8;
    if (cw < 4) cw = 8;
    x0 = win[54]; y0 = win[55];
    cols = (*(WORD *)(win + 8) - win[54] - win[56]) / cw;
    rows = (*(WORD *)(win + 10) - win[55] - win[57]) / lh;
    if (cols > 255) cols = 255;
    SetDrMd(rp, 1);                                     /* JAM2: each row paints its own background */

    SetTaskPri(FindTask(0), 21);
    for (int s = 0; s < NSRC; s++) { volatile const char *r; ULONG size, wr; if (ring_info(s, &r, &size, &wr)) seen[s] = wr > size ? wr - size : 0; }
    for (int s = 0; s < NSRC; s++) pull(s);
    scan_memory();
    ULONG port_mask = 1u << ((struct MsgPort *)*(APTR *)(win + 86))->mp_SigBit, tmask = 1u << tp->mp_SigBit;
    int quit = 0, tick = 0;
    while (!quit) {
        if (!paused) {
            for (int s = 0; s < NSRC; s++) pull(s);
            if (tick % 20 == 0) scan_memory();          /* every 10 s: devices loaded meanwhile */
            snapshot();
            to_board();
        }
        draw();
        tick++;
        tr->io.io_Command = TR_ADDREQUEST; tr->tv_secs = 0; tr->tv_micro = 500000;
        SendIO(tr);
        for (;;) {
            ULONG got = Wait(port_mask | tmask | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C) quit = 1;
            UBYTE *m;
            while ((m = GetMsg_(*(APTR *)(win + 86)))) {
                ULONG cls = *(ULONG *)(m + 20); UWORD code = *(UWORD *)(m + 24);
                ReplyMsg(m);
                if (cls == IDCMP_CLOSEWINDOW) quit = 1;
                else if (cls == IDCMP_VANILLAKEY) {
                    if (code == 'q' || code == 'Q' || code == 27) quit = 1;
                    else if (code == ' ') paused = !paused;
                    else if (code >= '1' && code <= '5') filter = code - '1';
                    else if (code == 't' || code == 'T') ntrows = ntrows == 14 ? 30 : ntrows == 30 ? 0 : 14;
                    draw();
                }
            }
            if (quit || CheckIO(tr)) break;
        }
        if (!CheckIO(tr)) AbortIO(tr);
        WaitIO(tr);
    }
    CloseWindow(win);
out:
    if (timer_ok) CloseDevice(tr);
    if (tr) DeleteIORequest(tr);
    if (tp) DeleteMsgPort(tp);
    if (INTUI) CloseLibrary(INTUI);
    if (GFX) CloseLibrary(GFX);
    CloseLibrary(dos);
    return 0;
}
