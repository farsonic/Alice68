/* SPDX-License-Identifier: GPL-2.0-only
 * alice68net.device - Alice68's network board as a netdev device for
 * rondoval's lwip-amiga stack.
 *
 * The network hardware is the kernel's: CPU1 runs the Pi 4's WiFi (unit 0)
 * and its Gigabit Ethernet (unit 1), and shares a 128 KB Zorro II
 * board with AmigaOS (alice_net.h). This device only moves frames between
 * the board's slots and the stack, and passes commands (join, scan, ...) to
 * the kernel; the level-2 interrupt says when the kernel has something new.
 *
 * netdev rather than SANA-II: the stack and the driver call each other
 * directly, with no IORequest per frame - the faster of the two.
 * netdev.h is lwip-amiga's (BSD-3-Clause). */
#include "../wifi/wifios.h"
#include "../wifi/netdev.h"
#include "../wifi/wifi_api.h"
#include "../../third_party/Emu68/src/alice/alice_net.h"

#define DEVNAME     "alice68net.device"
#define VERSION     2
#define REVISION    7
#define IDSTRING    "alice68net 2.7 (07.11.2026)\r\n"

#define BUFSZ       2048            /* one receive buffer: HEADROOM, the frame, spare room after it */
#define HEADROOM    64              /* bytes before the frame, as genet.device leaves (its status block):
                                       a stack may build a reply in place and reach in front of it */
#define FRAMEMAX    1536
#define TXRING      64
#define MAXSEGS     4
#define RXBATCH     16
#define NQ          16              /* kernel commands waiting */
#define SIGF_SINGLE (1u << 4)
#define INTB_PORTS  3
#define INTB_VERTB  5

ACHECK(sizeof(struct an_hdr) == 64);
ACHECK(sizeof(struct an_fw) == 72 && sizeof(struct an_scan) == 56);
ACHECK(sizeof(struct an_join) == 108 && sizeof(struct WifiJoin) == 108);
ACHECK(__builtin_offsetof(struct an_unit, arg) == 440);

struct txent { APTR cookie; UWORD nseg; struct NetDevSg sg[MAXSEGS]; };
struct kcmd { ULONG cmd; struct IOStdReq *io; UBYTE arg[108]; UWORD arglen; };

struct NetBase;
struct NUnit {                      /* one unit of the board: 0 WiFi, 1 Ethernet */
    struct Unit unit;               /* io_Unit points here */
    struct NetBase *nb;
    UWORD   idx;
    /* kernel commands, one at a time */
    struct kcmd q[NQ];
    UWORD   qhead, qtail;
    UBYTE   busy, pad4;
    ULONG   busy_seq, busy_t0;
    /* netdev */
    UBYTE   attached, started, link, pad5;
    struct IOStdReq *attach_io;     /* waiting for the hardware's MAC */
    ULONG   attach_t0;
    APTR    stackctx;
    const struct NetDevStackOps *sops;
    UBYTE  *pool;
    ULONG   npool, nfree, poolsize;
    UWORD  *freelist;
    struct txent tx[TXRING];
    volatile ULONG tx_head, tx_tail;
    APTR    done[TXRING];
    ULONG   ndone;
    struct NetDevRxDesc rxd[RXBATCH];
    ULONG   nrxd, rxbatch_max;
    ULONG   tx_drop, rx_nobuf, bad_release, bad_tx;
    /* receive: the board's slots, handed to the stack where they are (zero-copy) */
    ULONG   rx_next;                /* the next slot to hand up; rx_tail (the kernel's) trails it */
    UBYTE   held[AN_RX_SLOTS];      /* 1: the stack has this slot's frame */
    UBYTE   zc, pad6;               /* 1: zero-copy; 0: copy (the stack keeps buffers) */
    ULONG   zc_frames, copied_frames;
};

struct NetBase {
    struct Library lib;
    UWORD   pad0;
    APTR    seglist;                /* at 36: Alice68-Monitor finds the code through it */
    struct MsgPort port;            /* both units' requests */
    volatile UBYTE *b;              /* the board */
    struct Task *task, *waiter;
    ULONG   sig_port, sig_irq, sig_kick;
    volatile UBYTE running, alive, pad1, pad2;
    ULONG   sem[12];                /* SignalSemaphore: one opener starts the process */
    struct Interrupt irq_ports, irq_vbl;
    UBYTE   irqs_on, pad3;
    ULONG   ticks;
    struct NUnit u[AN_UNITS];
};
ACHECK(__builtin_offsetof(struct NetBase, seglist) == 36);

static struct NetBase *g_nb;
void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }
static void scpy(char *d, const volatile char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }

/* one line to the kernel's log (aga_glue forwards "[alice68" serial lines): only for things
   that should never happen, and only the first few */
static void plog(const char *s, ULONG v)
{
    static const char hx[] = "0123456789abcdef";
    for (const char *p = "[alice68net] "; *p; p++) RawPutChar(*p);
    for (; *s; s++) RawPutChar(*s);
    RawPutChar(' ');
    for (int i = 28; i >= 0; i -= 4) RawPutChar(hx[(v >> i) & 15]);
    RawPutChar('\n');
}

static inline volatile struct an_hdr *H(struct NetBase *n) { return (volatile struct an_hdr *)n->b; }
static inline volatile struct an_unit *U(struct NUnit *n) { return (volatile struct an_unit *)(n->nb->b + AN_OFF_UNIT(n->idx)); }
static void barrier(void) { __asm__ volatile("nop" ::: "memory"); }

/* ---- the kernel's commands --------------------------------------------------- */
static void q_add(struct NUnit *n, ULONG cmd, const void *arg, UWORD len, struct IOStdReq *io)
{
    if ((UWORD)(n->qtail - n->qhead) >= NQ) { if (io) { io->io.io_Error = NDERR_BUSY; ReplyMsg(io); } return; }
    struct kcmd *k = &n->q[n->qtail % NQ];
    k->cmd = cmd; k->io = io; k->arglen = len > sizeof k->arg ? sizeof k->arg : len;
    if (arg) memcpy(k->arg, arg, k->arglen);
    n->qtail++;
}
static void scan_copy(struct NUnit *n, struct IOStdReq *io)
{
    struct WifiScanEntry *e = io->io_Data;
    volatile struct an_scan *t = (volatile struct an_scan *)(n->nb->b + AN_OFF_SCAN);
    ULONG max = io->io_Length / sizeof *e, cnt = U(n)->scan_count, k = 0;
    if (cnt > AN_SCAN_MAX) cnt = AN_SCAN_MAX;
    for (; k < cnt && k < max; k++) {
        for (int i = 0; i < 6; i++) e[k].we_Bssid[i] = t[k].bssid[i];
        scpy(e[k].we_Ssid, t[k].ssid, sizeof e[k].we_Ssid);
        e[k].we_Rssi = (WORD)t[k].rssi;
        e[k].we_Channel = (UWORD)t[k].channel;
        e[k].we_Security = (UWORD)t[k].security;
    }
    io->io_Actual = k;
}
static void q_run(struct NUnit *n)
{
    volatile struct an_unit *u = U(n);
    if (n->busy) {
        barrier();
        int done = u->cmd_done == n->busy_seq;
        /* the kernel may be busy starting the chip for some seconds before it reads a command */
        int late = n->nb->ticks - n->busy_t0 > (n->q[n->qhead % NQ].cmd == AN_CMD_SCAN ? 45u * 50 : 30u * 50);
        if (!done && !late) return;
        barrier();                                  /* cmd_result and the scan table after cmd_done */
        struct kcmd *k = &n->q[n->qhead % NQ];
        if (k->io) {
            k->io->io.io_Error = done ? (u->cmd_result ? (BYTE)IOERR_OPENFAIL : 0) : (BYTE)NDERR_BUSY;
            if (done && !u->cmd_result && k->cmd == AN_CMD_SCAN) scan_copy(n, k->io);
            ReplyMsg(k->io);
        }
        n->busy = 0;
        n->qhead++;
    }
    if (n->qhead == n->qtail) return;
    struct kcmd *k = &n->q[n->qhead % NQ];
    for (unsigned i = 0; i < 128; i++) u->arg[i] = i < k->arglen ? k->arg[i] : 0;
    u->cmd = k->cmd;
    barrier();
    n->busy_seq = u->cmd_seq + 1;
    u->cmd_seq = n->busy_seq;
    n->busy = 1;
    n->busy_t0 = n->nb->ticks;
}

/* ---- receive ------------------------------------------------------------------ */
static UBYTE *pool_get(struct NUnit *n)
{
    UBYTE *b = 0;
    Disable();
    if (n->nfree && n->pool) b = n->pool + (ULONG)n->freelist[--n->nfree] * BUFSZ;
    Enable();
    return b;
}
static void pool_put(struct NUnit *n, UBYTE *b)
{
    /* a buffer that is not ours, or one given back twice, would push the free list past its end
       (into whoever's memory follows the pool): refuse it, count it, say so */
    ULONG off = (ULONG)(b - n->pool);
    int bad = 0;
    Disable();
    if (!n->pool || off >= n->npool * BUFSZ || off % BUFSZ || n->nfree >= n->npool) bad = 1;
    else n->freelist[n->nfree++] = (UWORD)(off / BUFSZ);
    Enable();
    if (bad && ++n->bad_release <= 5) plog("RxRelease of a buffer that is not out:", (ULONG)b);
}
static void pdec(ULONG v) { char t[12]; int k = 0; do t[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 11); while (k) RawPutChar(t[--k]); }
static void pstr(const char *s) { while (*s) RawPutChar(*s++); }
/* ICMP frames (ping) on their way to the stack, one line each, with net.trace=1 in
   cmdline.txt: with the kernel's own "rx ICMP" lines this shows where a reply is lost */
static void trace_icmp(struct NUnit *n, const struct NetDevRxDesc *r, int taken)
{
    const UBYTE *f = r->nrd_Data;
    if (!H(n->nb)->trace || r->nrd_Len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00 || f[14 + 9] != 1) return;
    const UBYTE *ic = f + 14 + (f[14] & 15) * 4;
    pstr("[alice68net] unit "); pdec(n->idx); pstr(" ICMP type "); pdec(ic[0]);
    pstr(" seq "); pdec((ULONG)ic[6] << 8 | ic[7]); pstr(" len "); pdec(r->nrd_Len);
    pstr(taken ? " handed to the stack\n" : " REFUSED by the stack (no buffer)\n");
}
/* Zero-copy receive. A frame is handed to the stack in its board slot; the slot goes back
   to the kernel (rx_tail) once the stack releases it, and the tail only moves over released
   slots, in order. Riptide copies and releases inside nso_RxInput, so it costs no copy here.
   A stack that keeps buffers (lwip-amiga holds a receive window) would hold the board's 16
   slots: the first time a slot is still held after nso_RxInput, this attachment copies. */
static volatile UBYTE *slot_data(struct NUnit *n, ULONG i)
{
    return ((volatile struct an_slot *)(n->nb->b + AN_OFF_RX(n->idx) + (i % AN_RX_SLOTS) * AN_SLOT_BYTES))->data;
}
static int slot_of(struct NUnit *n, APTR p)        /* -1: not one of this unit's slots */
{
    ULONG base = (ULONG)(n->nb->b + AN_OFF_RX(n->idx)), off = (ULONG)p - base;
    if (off >= AN_RX_SLOTS * AN_SLOT_BYTES || off % AN_SLOT_BYTES != 8) return -1;
    return (int)(off / AN_SLOT_BYTES);
}
static void slots_return(struct NUnit *n)          /* the tail over every released slot */
{
    volatile struct an_unit *u = U(n);
    Disable();
    while ((LONG)(n->rx_next - u->rx_tail) > 0 && !n->held[u->rx_tail % AN_RX_SLOTS]) { barrier(); u->rx_tail++; }
    Enable();
}
static void slot_release(struct NUnit *n, int k)
{
    Disable();
    if (n->held[k]) n->held[k] = 0;
    else if (++n->bad_release <= 5) plog("RxRelease of a slot that is not out:", (ULONG)k);
    Enable();
    slots_return(n);
}
static void rx_release(struct NUnit *n, APTR cookie)
{
    int k = slot_of(n, cookie);
    if (k >= 0) slot_release(n, k); else pool_put(n, cookie);
}

static void rx_flush(struct NUnit *n)
{
    if (!n->nrxd) return;
    ULONG c = n->nrxd, took = 0;
    if (n->attached && n->started) took = n->sops->nso_RxInput(n->stackctx, n->rxd, c);
    for (ULONG i = 0; i < c; i++) trace_icmp(n, &n->rxd[i], i < took);
    for (ULONG i = took; i < c; i++) rx_release(n, n->rxd[i].nrd_Cookie);
    if (n->zc)
        for (ULONG i = 0; i < took; i++) {
            int k = slot_of(n, n->rxd[i].nrd_Cookie);
            if (k >= 0 && n->held[k]) {
                n->zc = 0;                           /* the stack keeps buffers: copy from now on */
                plog("the stack keeps receive buffers: copying frames from now on, slots held", 0);
                break;
            }
        }
    n->nrxd = 0;
}
static void rx_all(struct NUnit *n)
{
    volatile struct an_unit *u = U(n);
    /* the kernel starts a new session at 0 (after a 68k reset); never run ahead of it */
    if ((LONG)(u->rx_head - n->rx_next) < 0 || (LONG)(n->rx_next - u->rx_tail) < 0) {
        Disable(); n->rx_next = u->rx_tail; memset(n->held, 0, sizeof n->held); Enable();
    }
    while (n->rx_next != u->rx_head) {
        barrier();
        ULONG k = n->rx_next % AN_RX_SLOTS;
        volatile struct an_slot *s = (volatile struct an_slot *)(n->nb->b + AN_OFF_RX(n->idx) + k * AN_SLOT_BYTES);
        ULONG len = s->len;
        int ok = n->attached && n->started && len <= FRAMEMAX;
        if (ok && n->zc) {
            struct NetDevRxDesc *r = &n->rxd[n->nrxd++];
            r->nrd_Data = (APTR)slot_data(n, k);
            r->nrd_Len = len;
            r->nrd_Flags = s->flags & AN_SF_BCAST ? NDRF_BCAST : s->flags & AN_SF_MCAST ? NDRF_MCAST : 0;
            r->nrd_CsumRaw = 0;
            r->nrd_VlanTci = 0;
            r->nrd_Cookie = r->nrd_Data;
            n->held[k] = 1;
            n->zc_frames++;
            n->rx_next++;
            if (n->nrxd >= n->rxbatch_max) rx_flush(n);
            continue;
        }
        UBYTE *buf = ok ? pool_get(n) : 0;
        if (buf) {
            const volatile ULONG *src = (const volatile ULONG *)s->data;
            ULONG *dst = (ULONG *)(buf + HEADROOM);
            for (ULONG i = 0; i < (len + 3) / 4; i++) dst[i] = src[i];
            struct NetDevRxDesc *r = &n->rxd[n->nrxd++];
            r->nrd_Data = buf + HEADROOM;
            r->nrd_Len = len;
            r->nrd_Flags = s->flags & AN_SF_BCAST ? NDRF_BCAST : s->flags & AN_SF_MCAST ? NDRF_MCAST : 0;
            r->nrd_CsumRaw = 0;
            r->nrd_VlanTci = 0;
            r->nrd_Cookie = buf;
            n->copied_frames++;
        } else n->rx_nobuf++;
        n->rx_next++;                               /* copied or dropped: the slot is free again */
        if (n->nrxd >= n->rxbatch_max) rx_flush(n);
    }
    rx_flush(n);
    slots_return(n);
}

/* ---- transmit ------------------------------------------------------------------ */
static void tx_done_flush(struct NUnit *n)
{
    if (!n->ndone) return;
    if (n->attached && n->sops->nso_TxDone) n->sops->nso_TxDone(n->stackctx, n->done, n->ndone);
    n->ndone = 0;
}
static void tx_all(struct NUnit *n)
{
    volatile struct an_unit *u = U(n);
    while (n->tx_tail != n->tx_head && u->tx_head - u->tx_tail < AN_TX_SLOTS) {
        struct txent *t = &n->tx[n->tx_tail % TXRING];
        volatile struct an_slot *s = (volatile struct an_slot *)(n->nb->b + AN_OFF_TX(n->idx) + (u->tx_head % AN_TX_SLOTS) * AN_SLOT_BYTES);
        ULONG len = 0;
        for (UWORD i = 0; i < t->nseg; i++) {
            const UBYTE *p = t->sg[i].nsg_Data;
            ULONG l = t->sg[i].nsg_Len;
            if (len + l > AN_SLOT_BYTES - 8) { len = 0; break; }
            for (ULONG k = 0; k < l; k++) s->data[len + k] = p[k];
            len += l;
        }
        if (len && H(n->nb)->trace && t->sg[0].nsg_Len >= 14 + 20 + 8) {
            const UBYTE *f = t->sg[0].nsg_Data;
            if (f[12] == 0x08 && f[13] == 0x00 && f[14 + 9] == 1) {
                const UBYTE *ic = f + 14 + (f[14] & 15) * 4;
                pstr("[alice68net] unit "); pdec(n->idx); pstr(" ICMP type "); pdec(ic[0]);
                pstr(" seq "); pdec((ULONG)ic[6] << 8 | ic[7]); pstr(" len "); pdec(len); pstr(" sent to the kernel\n");
            }
        }
        if (len) {
            s->len = len;
            s->flags = 0;
            barrier();
            u->tx_head++;
        } else n->tx_drop++;
        n->done[n->ndone++] = t->cookie;
        n->tx_tail++;
        if (n->ndone == TXRING) tx_done_flush(n);
    }
    tx_done_flush(n);
}
static void tx_drop_all(struct NUnit *n)
{
    while (n->tx_tail != n->tx_head) {
        n->done[n->ndone++] = n->tx[n->tx_tail % TXRING].cookie;
        n->tx_tail++;
        n->tx_drop++;
        if (n->ndone == TXRING) tx_done_flush(n);
    }
    tx_done_flush(n);
}

/* ---- netdev entries (the stack, any task) ------------------------------------------ */
static LONG ndo_TxSubmit(APTR ctx, const struct NetDevTxDesc *d, ULONG count)
{
    struct NUnit *n = ctx;
    ULONG k = 0;
    Disable();
    while (k < count && n->tx_head - n->tx_tail < TXRING) {
        struct txent *t = &n->tx[n->tx_head % TXRING];
        t->cookie = d[k].ntd_Cookie;
        /* a descriptor we cannot send is taken and completed as dropped (nseg 0): a short
           return would tell the stack to wait for a TxDone that never makes room for it */
        UWORD ns = d[k].ntd_NumSegs;
        if (ns == 0 || ns > MAXSEGS) { ns = 0; n->bad_tx++; }
        for (UWORD i = 0; i < ns; i++) {
            t->sg[i] = d[k].ntd_Segs[i];
            if (!t->sg[i].nsg_Data || t->sg[i].nsg_Len > FRAMEMAX) { ns = 0; n->bad_tx++; break; }
        }
        t->nseg = ns;
        n->tx_head++;
        k++;
    }
    Enable();
    return (LONG)k;
}
static VOID ndo_TxKick(APTR ctx) { struct NUnit *n = ctx; Disable(); if (n->nb->task) Signal(n->nb->task, n->nb->sig_kick); Enable(); }
static VOID ndo_RxRelease(APTR ctx, APTR cookie) { rx_release(ctx, cookie); }
static APTR ndo_DmaAlloc(APTR ctx, ULONG size, ULONG align)
{
    (void)ctx;
    if (align < 4) align = 4;
    ULONG total = size + align + 8;
    UBYTE *m = AllocMem(total, MEMF_PUBLIC);
    if (!m) return 0;
    UBYTE *p = (UBYTE *)(((ULONG)m + 8 + align - 1) & ~(align - 1));
    ((ULONG *)p)[-1] = (ULONG)m;
    ((ULONG *)p)[-2] = total;
    return p;
}
static VOID ndo_DmaFree(APTR ctx, APTR p, ULONG size) { (void)ctx; (void)size; if (p) FreeMem((APTR)((ULONG *)p)[-1], ((ULONG *)p)[-2]); }
static const struct NetDevDrvOps drv_ops = { ndo_TxSubmit, ndo_TxKick, ndo_RxRelease, ndo_DmaAlloc, ndo_DmaFree };

static void link_check(struct NUnit *n)
{
    volatile struct an_unit *u = U(n);
    UBYTE up = u->link ? 1 : 0;
    if (up == n->link) return;
    n->link = up;
    if (n->attached && n->started && n->sops->nso_LinkChange) {
        struct NetDevLinkState ls = { up ? NDLF_UP | NDLF_FULL_DUPLEX : 0, up ? (u->speed_mbps ? u->speed_mbps : 54) : 0 };
        n->sops->nso_LinkChange(n->stackctx, &ls);
    }
}

/* ---- netdev commands -------------------------------------------------------------- */
static void pool_free(struct NUnit *n)
{
    if (n->pool) FreeMem(n->pool, n->poolsize);
    n->pool = 0; n->freelist = 0; n->npool = n->nfree = 0;
}
static int mac_known(struct NUnit *n) { volatile UBYTE *m = U(n)->mac; return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0; }
static LONG do_attach(struct NUnit *n, struct IOStdReq *io)
{
    struct NetDevAttach *a = io->io_Data;
    if (!a || io->io_Length < sizeof *a) return NDERR_BADPARAMS;
    if (n->attached) return NDERR_BUSY;
    if (a->nda_AbiVersion < 1) return NDERR_BADVERSION;
    ULONG c = a->nda_RxHoldReq ? a->nda_RxHoldReq + 32u : 96u;
    if (c > 256) c = 256;
    if (c < 48) c = 48;
    n->poolsize = c * BUFSZ + c * 2;
    n->pool = AllocMem(n->poolsize, MEMF_PUBLIC);
    if (!n->pool) return NDERR_NOMEM;
    n->npool = c;
    n->freelist = (UWORD *)(n->pool + c * BUFSZ);
    for (ULONG i = 0; i < c; i++) n->freelist[i] = (UWORD)i;
    n->nfree = c;
    n->rxbatch_max = a->nda_RxBatch && a->nda_RxBatch < RXBATCH ? a->nda_RxBatch : RXBATCH;
    n->stackctx = a->nda_StackCtx;
    n->sops = a->nda_StackOps;
    a->nda_AbiVersion = 1;
    a->nda_DrvCtx = n;
    a->nda_DrvOps = &drv_ops;
    struct NetDevCaps *k = &a->nda_Caps;
    memset(k, 0, sizeof *k);
    k->ndc_AbiVersion = NETDEV_ABI_VERSION;
    k->ndc_Mtu = 1500;
    for (int i = 0; i < 6; i++) k->ndc_Mac[i] = U(n)->mac[i];
    k->ndc_TxMaxSegs = MAXSEGS;
    k->ndc_Features = NDCF_LINK_EVENTS;              /* multicast: all of it (no exact filter) */
    k->ndc_TxRingSlots = TXRING;
    k->ndc_TxInFlightMax = TXRING;
    k->ndc_RxPoolBufs = c;
    n->tx_head = n->tx_tail = 0;
    n->ndone = n->nrxd = 0;
    n->rx_next = U(n)->rx_tail;
    memset(n->held, 0, sizeof n->held);
    n->zc = 1;
    n->attached = 1;
    n->started = 0;
    n->link = 0;
    return 0;
}
static void fill_status(struct NUnit *n, struct WifiStatus *s)
{
    volatile struct an_unit *u = U(n);
    memset(s, 0, sizeof *s);
    s->ws_Version = WIFI_API_VERSION;
    s->ws_State = (UWORD)u->state;
    s->ws_Chip = (UWORD)u->chip; s->ws_ChipRev = (UWORD)u->chiprev;
    for (int i = 0; i < 6; i++) { s->ws_Mac[i] = u->mac[i]; s->ws_Bssid[i] = u->bssid[i]; }
    scpy(s->ws_Ssid, u->ssid, sizeof s->ws_Ssid);
    s->ws_Rssi = (WORD)u->rssi;
    s->ws_Channel = (UWORD)u->channel;
    s->ws_RateMbps = (UWORD)u->speed_mbps;
    s->ws_Security = (UWORD)u->security;
    s->ws_Interrupts = 0;
    s->ws_SdMhz = (UWORD)u->sdmhz;
    s->ws_Attached = n->attached;
    s->ws_RxPackets = u->rx_pkts; s->ws_TxPackets = u->tx_pkts;
    s->ws_RxBytes = u->rx_bytes; s->ws_TxBytes = u->tx_bytes;
    s->ws_RxDropped = u->rx_drop + n->rx_nobuf; s->ws_TxErrors = u->tx_err + n->tx_drop;
    s->ws_SdErrors = u->sd_errors; s->ws_Irqs = u->polls;
    scpy(s->ws_FwVersion, u->fwver, sizeof s->ws_FwVersion);
    scpy(s->ws_Message, u->msg, sizeof s->ws_Message);
}
static void log_copy(struct NUnit *n, struct IOStdReq *io)
{
    char *d = io->io_Data;
    ULONG wr = H(n->nb)->log_wr, have = wr < AN_LOG_BYTES ? wr : AN_LOG_BYTES, max = io->io_Length;
    if (have > max) have = max;
    volatile const char *r = (volatile const char *)(n->nb->b + AN_OFF_LOG);
    for (ULONG i = 0; i < have; i++) d[i] = r[(wr - have + i) % AN_LOG_BYTES];
    io->io_Actual = have;
}

/* 1: the request is kept (answered later) */
static int command(struct NUnit *n, struct IOStdReq *io)
{
    LONG err = 0;
    switch (io->io.io_Command) {
    case NETDEV_CMD_ATTACH:
        if (n->attach_io) { err = NDERR_BUSY; break; }
        /* the Ethernet starts when a stack takes it (also after "Ethernet off", or a start that failed) */
        if (n->idx == 1 && (U(n)->state == AN_ST_OFF || U(n)->state == AN_ST_FAILED)) q_add(n, AN_CMD_START, 0, 0, 0);
        if (!mac_known(n) || n->busy || n->qhead != n->qtail ||
            (n->idx == 1 && U(n)->state != AN_ST_IDLE && U(n)->state != AN_ST_CONNECTED)) {
            n->attach_io = io; n->attach_t0 = n->nb->ticks; return 1;   /* still starting */
        }
        err = do_attach(n, io);
        break;
    case NETDEV_CMD_DETACH:
        if (!n->attached) { err = NDERR_NOTATTACHED; break; }
        q_add(n, AN_CMD_RXOFF, 0, 0, 0);
        tx_drop_all(n);
        n->attached = n->started = 0;
        pool_free(n);
        break;
    case NETDEV_CMD_START:
        if (!n->attached) { err = NDERR_NOTATTACHED; break; }
        U(n)->rx_tail = U(n)->rx_head;              /* old frames: skipped (the tail is ours) */
        n->rx_next = U(n)->rx_head;
        memset(n->held, 0, sizeof n->held);
        n->zc = 1;
        n->started = 1;
        q_add(n, AN_CMD_RXON, 0, 0, 0);
        n->link = 0xff;                             /* tell the stack the link state now */
        link_check(n);
        break;
    case NETDEV_CMD_STOP:
        if (!n->attached) { err = NDERR_NOTATTACHED; break; }
        q_add(n, AN_CMD_RXOFF, 0, 0, 0);
        rx_flush(n);
        n->started = 0;
        tx_drop_all(n);
        break;
    case NETDEV_CMD_SET_RXFILTER: {
        struct NetDevRxFilter *f = io->io_Data;
        if (!f) { err = NDERR_BADPARAMS; break; }
        UBYTE a[4] = { 0, 0, 0, 0 };
        if (f->ndrx_Flags & NDFF_PROMISC) a[3] |= AN_FILT_PROMISC;
        if ((f->ndrx_Flags & NDFF_ALLMULTI) || f->ndrx_NumMcast) a[3] |= AN_FILT_ALLMULTI;
        q_add(n, AN_CMD_FILTER, a, 4, 0);
        break;
    }
    case NETDEV_CMD_SET_COALESCE: break;
    case NETDEV_CMD_GET_STATS: {
        struct NetDevStats *s = io->io_Data;
        volatile struct an_unit *u = U(n);
        if (!s || io->io_Length < sizeof *s) { err = NDERR_BADPARAMS; break; }
        memset(s, 0, sizeof *s);
        s->nds_RxPackets.ndu_Lo = u->rx_pkts; s->nds_RxBytes.ndu_Lo = u->rx_bytes;
        s->nds_RxDropped.ndu_Lo = u->rx_drop + n->rx_nobuf; s->nds_TxPackets.ndu_Lo = u->tx_pkts;
        s->nds_TxBytes.ndu_Lo = u->tx_bytes; s->nds_TxErrors.ndu_Lo = u->tx_err;
        s->nds_TxDropped.ndu_Lo = n->tx_drop; s->nds_RxErrors.ndu_Lo = u->sd_errors;
        break;
    }
    case NETDEV_CMD_GET_LINK: {
        struct NetDevLinkState *l = io->io_Data;
        if (!l) { err = NDERR_BADPARAMS; break; }
        l->ndls_Flags = U(n)->link ? NDLF_UP | NDLF_FULL_DUPLEX : 0;
        l->ndls_SpeedMbps = U(n)->link ? (U(n)->speed_mbps ? U(n)->speed_mbps : (n->idx ? 1000 : 54)) : 0;
        break;
    }
    case NETDEV_CMD_SET_MAC: {                       /* UBYTE[6] */
        const UBYTE *m = io->io_Data;
        if (!m || io->io_Length < 6 || (m[0] & 1)) { err = NDERR_BADPARAMS; break; }
        UBYTE a[7] = { m[0], m[1], m[2], m[3], m[4], m[5], 0 };
        q_add(n, AN_CMD_SETMAC, a, 7, io);
        return 1;
    }
    case NET_CMD_ETHCFG:
        if (n->idx != 1) { err = IOERR_NOCMD; break; }
        if (!io->io_Data || io->io_Length < sizeof(struct NetEthCfg)) { err = NDERR_BADPARAMS; break; }
        q_add(n, AN_CMD_ETHCFG, io->io_Data, sizeof(struct NetEthCfg), io);
        return 1;
    case NET_CMD_WIFICFG: {
        if (n->idx != 0) { err = IOERR_NOCMD; break; }
        if (!io->io_Data || io->io_Length < 4) { err = NDERR_BADPARAMS; break; }
        ULONG a[2] = { 0, *(const ULONG *)io->io_Data };
        q_add(n, AN_CMD_WIFICFG, a, 8, io);
        return 1;
    }
    case NET_CMD_SETMAC: {                           /* UBYTE[8]: address, NETMAC_* */
        const UBYTE *m = io->io_Data;
        if (!m || io->io_Length < 8 || m[6] > NETMAC_FACTORY || (m[6] != NETMAC_FACTORY && (m[0] & 1))) { err = NDERR_BADPARAMS; break; }
        UBYTE a[7] = { 0, 0, 0, 0, 0, 0, 0 };
        if (m[6] != NETMAC_FACTORY) for (int i = 0; i < 6; i++) a[i] = m[i];
        a[6] = m[6] == NETMAC_OUI;
        q_add(n, AN_CMD_SETMAC, a, 7, io);
        return 1;
    }
    case WIFI_CMD_START: q_add(n, AN_CMD_START, 0, 0, io); return 1;
    case WIFI_CMD_JOIN:
        if (n->idx != 0) { err = IOERR_NOCMD; break; }
        if (!io->io_Data || io->io_Length < sizeof(struct WifiJoin)) { err = NDERR_BADPARAMS; break; }
        q_add(n, AN_CMD_JOIN, io->io_Data, sizeof(struct WifiJoin), io);
        return 1;
    case WIFI_CMD_RECONNECT: {                      /* a JOIN without a name: the last network again */
        if (n->idx != 0) { io->io.io_Error = IOERR_NOCMD; return 0; }
        struct WifiJoin j;
        memset(&j, 0, sizeof j);
        q_add(n, AN_CMD_JOIN, &j, sizeof j, io);
        return 1;
    }
    case WIFI_CMD_SCAN:
        if (n->idx != 0) { err = IOERR_NOCMD; break; }
        if (!io->io_Data) { err = NDERR_BADPARAMS; break; }
        q_add(n, AN_CMD_SCAN, 0, 0, io);
        return 1;
    case WIFI_CMD_DISCONNECT: q_add(n, AN_CMD_DISCONNECT, 0, 0, io); return 1;
    case WIFI_CMD_OFF:
        if (n->attached) { err = NDERR_BUSY; break; }
        q_add(n, AN_CMD_OFF, 0, 0, io);
        return 1;
    default: err = IOERR_NOCMD; break;
    }
    io->io.io_Error = (BYTE)err;
    return 0;
}

/* ---- interrupts: the kernel's level-2 signal, and every frame as a backstop ------- */
void dev_irq(struct NetBase *n)
{
    if (!n->b || !n->task) return;
    volatile struct an_hdr *h = H(n);
    if (h->irq) { h->irq = 0; Signal(n->task, n->sig_irq); return; }
    for (int i = 0; i < AN_UNITS; i++) {
        struct NUnit *nu = &n->u[i];
        volatile struct an_unit *u = U(nu);
        if ((nu->started && u->rx_head != nu->rx_next) || (nu->busy && u->cmd_done == nu->busy_seq)) { Signal(n->task, n->sig_irq); return; }
    }
}

/* ---- the saved settings (Alice68-Network): ENV:alice68net.prefs ---------------------------
   MAC0= and MAC1= (unit 0 WiFi, unit 1 Ethernet): COMMODORE (Commodore's OUI 00:80:10 on the
   hardware's own last three bytes), an address aa:bb:cc:dd:ee:ff, or nothing (the kernel's
   choice: Commodore's OUI unless net.oui=off). ETH=adv,flags,speed (NET_CMD_ETHCFG's numbers,
   decimal) and WIFI=flags (AN_WC_*). Read once, when the process starts: before any stack can
   attach. */
static ULONG decv(const char **p)
{
    ULONG v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (ULONG)(*(*p)++ - '0');
    if (**p == ',') (*p)++;
    return v;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void mac_prefs(struct NetBase *n)
{
    APTR dos = OpenLibrary("dos.library", 37);
    if (!dos) return;
    static char buf[512];
    ULONG fh = DosOpen(dos, "ENV:alice68net.prefs", MODE_OLDFILE);
    if (!fh) fh = DosOpen(dos, "ENVARC:alice68net.prefs", MODE_OLDFILE);
    LONG len = fh ? DosRead(dos, fh, buf, sizeof buf - 1) : 0;
    if (fh) DosClose(dos, fh);
    CloseLibrary(dos);
    if (len <= 0) return;
    buf[len] = 0;
    for (char *l = buf; *l; ) {
        char *e = l; while (*e && *e != '\n') e++;
        if (l[0] == 'M' && l[1] == 'A' && l[2] == 'C' && (l[3] == '0' || l[3] == '1') && l[4] == '=') {
            struct NUnit *nu = &n->u[l[3] - '0'];
            char *v = l + 5;
            UBYTE a[7] = { 0, 0, 0, 0, 0, 0, 0 };
            int ok = 1;
            if ((v[0] | 0x20) == 'c') {                  /* COMMODORE */
                a[0] = (UBYTE)(AN_OUI_COMMODORE >> 16); a[1] = (UBYTE)(AN_OUI_COMMODORE >> 8); a[2] = (UBYTE)AN_OUI_COMMODORE; a[6] = 1;
            } else if (v < e && hexv(*v) >= 0) {
                for (int i = 0; i < 6 && ok; i++, v += 3) {
                    int h = hexv(v[0]), lo = hexv(v[1]);
                    if (h < 0 || lo < 0 || (i < 5 && v[2] != ':')) ok = 0;
                    else a[i] = (UBYTE)(h << 4 | lo);
                }
                if (a[0] & 1) ok = 0;
            } else ok = 0;                               /* nothing: the hardware's own, which it has */
            if (ok) q_add(nu, AN_CMD_SETMAC, a, 7, 0);
        } else if (l[0] == 'E' && l[1] == 'T' && l[2] == 'H' && l[3] == '=' && l[4] >= '0' && l[4] <= '9') {
            const char *v = l + 4;
            ULONG c[3];
            c[0] = decv(&v); c[1] = decv(&v); c[2] = decv(&v);
            q_add(&n->u[1], AN_CMD_ETHCFG, c, 12, 0);
        } else if (l[0] == 'W' && l[1] == 'I' && l[2] == 'F' && l[3] == 'I' && l[4] == '=' && l[5] >= '0' && l[5] <= '9') {
            const char *v = l + 5;
            ULONG c[2] = { 0, decv(&v) };
            q_add(&n->u[0], AN_CMD_WIFICFG, c, 8, 0);
        }
        l = *e ? e + 1 : e;
    }
}

/* ---- the process -------------------------------------------------------------------- */
static void net_proc(void)
{
    struct NetBase *n = g_nb;
    n->task = FindTask(0);
    LONG sb_port = AllocSignal(-1), sb_irq = AllocSignal(-1), sb_kick = AllocSignal(-1);
    n->sig_port = 1u << sb_port; n->sig_irq = 1u << sb_irq; n->sig_kick = 1u << sb_kick;
    struct MsgPort *tp = CreateMsgPort();
    struct timerequest *tr = tp ? CreateIORequest(tp, sizeof *tr) : 0;
    int timer_ok = tr && !OpenDevice("timer.device", 1, tr, 0);      /* UNIT_VBLANK */
    mac_prefs(n);                                   /* queued before anyone can attach */
    Forbid();
    n->port.mp_SigBit = (UBYTE)sb_port;
    n->port.mp_SigTask = n->task;
    n->port.mp_Flags = PA_SIGNAL;
    n->running = 1;
    n->alive = 1;
    Signal(n->waiter, SIGF_SINGLE);
    n->waiter = 0;
    Permit();
    n->irq_ports.is_Node.ln_Type = 2;
    n->irq_ports.is_Node.ln_Name = (char *)DEVNAME;
    n->irq_ports.is_Data = n;
    extern void stub_irq(void);
    n->irq_ports.is_Code = stub_irq;
    n->irq_vbl = n->irq_ports;
    AddIntServer(INTB_PORTS, &n->irq_ports);
    AddIntServer(INTB_VERTB, &n->irq_vbl);
    n->irqs_on = 1;
    H(n)->amiga_ver = VERSION << 16 | REVISION;
    ULONG tmask = timer_ok ? 1u << tp->mp_SigBit : 0;
    int tout = 0;
    for (;;) {
        if (timer_ok && !tout) {
            tr->io.io_Command = TR_ADDREQUEST; tr->tv_secs = 0; tr->tv_micro = 20000;
            SendIO(tr);
            tout = 1;
        }
        Wait(n->sig_port | n->sig_irq | n->sig_kick | tmask);
        if (tout && CheckIO(tr)) { WaitIO(tr); tout = 0; n->ticks++; }
        struct IOStdReq *io;
        while ((io = GetMsg(&n->port))) {
            struct NUnit *nu = (struct NUnit *)io->io.io_Unit;
            if (!command(nu, io)) ReplyMsg(io);
        }
        for (int i = 0; i < AN_UNITS; i++) {
            struct NUnit *nu = &n->u[i];
            if (nu->attach_io) {
                int ready = mac_known(nu) && !nu->busy && nu->qhead == nu->qtail     /* a MAC from the prefs first */
                         && (nu->idx == 0 || U(nu)->state == AN_ST_IDLE || U(nu)->state == AN_ST_CONNECTED);
                if (ready) { io = nu->attach_io; nu->attach_io = 0; io->io.io_Error = (BYTE)do_attach(nu, io); ReplyMsg(io); }
                else if (n->ticks - nu->attach_t0 > 30u * 50) { io = nu->attach_io; nu->attach_io = 0; io->io.io_Error = IOERR_OPENFAIL; ReplyMsg(io); }
            }
            q_run(nu);
            if (nu->attached) rx_all(nu);
            if (nu->attached && nu->started) tx_all(nu);
            link_check(nu);
        }
    }
}

/* ---- the device ------------------------------------------------------------------------- */
static void find_board(struct NetBase *n)
{
    if (n->b) return;
    APTR eb = OpenLibrary("expansion.library", 36);
    if (!eb) return;
    struct ConfigDev *cd = FindConfigDev(eb, 0, AN_MANUFACTURER, AN_PRODUCT);
    CloseLibrary(eb);
    if (!cd) return;
    volatile struct an_hdr *h = cd->cd_BoardAddr;
    if (h->magic != AN_MAGIC || h->version != AN_VERSION) return;
    n->b = (volatile UBYTE *)h;
}

ULONG dev_init(struct NetBase *n, APTR seglist)
{
    n->seglist = seglist;
    n->lib.lib_Node.ln_Type = NT_DEVICE;
    n->lib.lib_Node.ln_Name = (char *)DEVNAME;
    n->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    n->lib.lib_Version = VERSION;
    n->lib.lib_Revision = REVISION;
    n->lib.lib_IdString = (APTR)IDSTRING;
    n->port.mp_Node.ln_Type = NT_MSGPORT;
    n->port.mp_Flags = PA_IGNORE;
    NewList(&n->port.mp_MsgList);
    for (int i = 0; i < AN_UNITS; i++) {
        n->u[i].nb = n;
        n->u[i].idx = (UWORD)i;
        n->u[i].unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        n->u[i].unit.unit_MsgPort.mp_Flags = PA_IGNORE;
        NewList(&n->u[i].unit.unit_MsgPort.mp_MsgList);
    }
    InitSemaphore(n->sem);
    g_nb = n;
    return (ULONG)n;
}

LONG dev_open(struct NetBase *n, struct IOStdReq *io, ULONG unit, ULONG flags)
{
    (void)flags;
    io->io.io_Error = IOERR_OPENFAIL;
    find_board(n);
    /* unit 0 WiFi, unit 1 Ethernet: there when the kernel serves them (eth=off: no unit 1) */
    if (!n->b || unit >= AN_UNITS) return -1;
    struct NUnit *nu = &n->u[unit];
    if (U(nu)->kind == AN_KIND_NONE) return -1;
    n->lib.lib_OpenCnt++;
    ObtainSemaphore(n->sem);
    if (!n->running) {
        APTR dos = OpenLibrary("dos.library", 37);
        if (dos) {
            static const char pname[] = "alice68net";
            struct TagItem t[] = { { NP_Entry, (ULONG)net_proc }, { NP_Name, (ULONG)pname },
                                   { NP_StackSize, 8192 }, { NP_Priority, 5 },
                                   { NP_CurrentDir, 0 }, { NP_HomeDir, 0 }, { NP_CopyVars, 0 }, { TAG_DONE, 0 } };
            n->waiter = FindTask(0);
            SetSignal(0, SIGF_SINGLE);
            if (CreateNewProc(dos, t)) while (!n->running) Wait(SIGF_SINGLE);
            CloseLibrary(dos);
        }
    }
    ReleaseSemaphore(n->sem);
    if (!n->running) { n->lib.lib_OpenCnt--; return -1; }
    io->io.io_Unit = &nu->unit;
    io->io.io_Error = 0;
    nu->unit.unit_OpenCnt++;
    n->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return 0;
}

ULONG dev_close(struct NetBase *n, struct IOStdReq *io)
{
    struct NUnit *nu = (struct NUnit *)io->io.io_Unit;
    io->io.io_Unit = (struct Unit *)-1;
    io->io.io_Device = (APTR)-1;
    if ((ULONG)nu != (ULONG)-1 && nu && nu->unit.unit_OpenCnt) nu->unit.unit_OpenCnt--;
    if (n->lib.lib_OpenCnt) n->lib.lib_OpenCnt--;
    return 0;                       /* the process stays: the network keeps running */
}

ULONG dev_expunge(struct NetBase *n) { n->lib.lib_Flags |= LIBF_DELEXP; return 0; }

void dev_beginio(struct NetBase *n, struct IOStdReq *io)
{
    struct NUnit *nu = (struct NUnit *)io->io.io_Unit;
    io->io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->io.io_Error = 0;
    UWORD c = io->io.io_Command;
    if (c == NSCMD_DEVICEQUERY || c == WIFI_CMD_STATUS || c == WIFI_CMD_LOG) {
        static const UWORD cmds[] = {
            NSCMD_DEVICEQUERY, NETDEV_CMD_ATTACH, NETDEV_CMD_DETACH, NETDEV_CMD_START, NETDEV_CMD_STOP, NETDEV_CMD_SET_MAC, NET_CMD_SETMAC,
            NETDEV_CMD_SET_RXFILTER, NETDEV_CMD_SET_COALESCE, NETDEV_CMD_GET_STATS, NETDEV_CMD_GET_LINK,
            WIFI_CMD_STATUS, WIFI_CMD_SCAN, WIFI_CMD_LOG, WIFI_CMD_RECONNECT, WIFI_CMD_DISCONNECT, WIFI_CMD_OFF,
            WIFI_CMD_START, WIFI_CMD_JOIN, NET_CMD_ETHCFG, NET_CMD_WIFICFG, 0 };
        if (!io->io_Data) io->io.io_Error = NDERR_BADPARAMS;
        else if (c == NSCMD_DEVICEQUERY) {
            struct NSDeviceQueryResult *q = io->io_Data;
            if (io->io_Length < sizeof *q) io->io.io_Error = IOERR_NOCMD;
            else {
                q->DevQueryFormat = 0; q->SizeAvailable = sizeof *q;
                q->DeviceType = 0; q->DeviceSubType = 0;
                q->SupportedCommands = cmds;
                io->io_Actual = sizeof *q;
            }
        } else if (c == WIFI_CMD_STATUS) {
            if (io->io_Length < sizeof(struct WifiStatus)) io->io.io_Error = NDERR_BADPARAMS;
            else { fill_status(nu, io->io_Data); io->io_Actual = sizeof(struct WifiStatus); }
        } else log_copy(nu, io);
        if (!(io->io.io_Flags & IOF_QUICK)) ReplyMsg(io);
        return;
    }
    io->io.io_Flags &= (UBYTE)~IOF_QUICK;
    PutMsg(&n->port, io);
}

ULONG dev_abortio(struct NetBase *n, struct IOStdReq *io)
{
    Forbid();
    int done = 0;
    for (struct Node *x = n->port.mp_MsgList.lh_Head; x->ln_Succ; x = x->ln_Succ)
        if (x == (struct Node *)io) {
            Remove(x);
            io->io.io_Error = IOERR_ABORTED;
            ReplyMsg(io);
            done = 1;
            break;
        }
    /* already taken by the process: in a unit's command queue, or an attach waiting.
       The kernel's command itself goes on; only the request is answered. */
    for (int u = 0; u < AN_UNITS && !done; u++) {
        struct NUnit *nu = &n->u[u];
        for (UWORD i = nu->qhead; i != nu->qtail && !done; i++)
            if (nu->q[i % NQ].io == io) { nu->q[i % NQ].io = 0; io->io.io_Error = IOERR_ABORTED; ReplyMsg(io); done = 1; }
        if (!done && nu->attach_io == io) { nu->attach_io = 0; io->io.io_Error = IOERR_ABORTED; ReplyMsg(io); done = 1; }
    }
    Permit();
    return 0;
}

const char dev_name[] = DEVNAME;
const char dev_idstring[] = IDSTRING;
extern void stub_init(void), stub_open(void), stub_close(void), stub_expunge(void),
            stub_null(void), stub_beginio(void), stub_abortio(void);
const ULONG dev_functab[] = {
    (ULONG)stub_open, (ULONG)stub_close, (ULONG)stub_expunge, (ULONG)stub_null,
    (ULONG)stub_beginio, (ULONG)stub_abortio, (ULONG)-1
};
const ULONG dev_inittab[4] = { sizeof(struct NetBase), (ULONG)dev_functab, 0, (ULONG)stub_init };
