/* SPDX-License-Identifier: GPL-2.0-only
 * alice68wifi.device - the Raspberry Pi 4's built-in WiFi (Cypress CYW43455)
 * as a netdev network device for rondoval's lwip-amiga TCP/IP stack.
 *
 * One process owns the chip: it loads the firmware, brings the chip up over
 * SDIO (sdio.c, chip.c), runs the FullMAC protocol (fmac.c), joins the
 * network named in ENV:alice68wifi.prefs with the chip's own WPA supplicant,
 * and moves frames between the chip and the stack. The card interrupt
 * (through gic400.library) wakes it; without it, it polls.
 *
 * Written for Alice68 from Linux brcmfmac and OpenBSD bwfm (ISC) as
 * references; the netdev interface is lwip-amiga's (netdev.h, BSD-3-Clause). */
#include "wifios.h"
#include "netdev.h"
#include "wifi_api.h"
#include "bus.h"
#include "fmac.h"

#define DEVNAME     "alice68wifi.device"
#define VERSION     1
#define REVISION    0
#define IDSTRING    "alice68wifi 1.0 (06.11.2026)\r\n"

#define BUFSZ       2048            /* one frame buffer */
#define CTLMAX      1600            /* largest control payload */
#define TXRING      64
#define MAXSEGS     4
#define SCAN_MAX    40
#define RXBATCH     32
#define LOGSZ       4096
#define GIC_IRQ     (32 + 126)      /* SPI 126: the Arasan host (shared with EMMC2, which polls) */
#define SIGF_SINGLE (1u << 4)

struct txent { APTR cookie; UWORD nseg; struct NetDevSg sg[MAXSEGS]; };

struct WifiBase {
    struct Library lib;
    UWORD   pad0;
    APTR    seglist;
    struct Unit unit;
    APTR    dos, gic, mbox;
    struct Task *task, *waiter;
    ULONG   sig_irq, sig_kick, sig_port;
    volatile UBYTE quit, running;
    UBYTE   poll, irq_on, chip_up, pad1;
    struct Interrupt irq;
    volatile ULONG irqs, spurious;

    struct sdhost h;
    struct chip   c;
    UBYTE  *rxbuf, *txbuf, *ctlbuf, *iobuf;     /* BUFSZ each, one allocation */
    UBYTE   txseq, maxseq;
    UWORD   reqid;
    UWORD   ctl_wait;
    volatile UBYTE ctl_got;
    LONG    ctl_status;
    UBYTE  *ctl_out;
    ULONG   ctl_outmax;

    /* netdev */
    UBYTE   attached, started;
    APTR    stackctx;
    const struct NetDevStackOps *sops;
    UBYTE  *pool;                   /* npool * BUFSZ */
    ULONG   npool, nfree, poolsize;
    UWORD  *freelist;
    struct txent tx[TXRING];
    volatile ULONG tx_head, tx_tail;    /* stack produces at head, task consumes at tail */
    APTR    done[TXRING];
    ULONG   ndone;
    struct NetDevRxDesc rxd[RXBATCH];
    ULONG   nrxd;
    UWORD   rxbatch_max;
    struct NetDevStats st;
    ULONG   rx_pkts, rx_bytes, tx_pkts, tx_bytes, rx_drop, tx_err, tx_drop;

    /* WiFi */
    UBYTE   mac[6];
    char    fwver[96];
    char    caps[256];
    char    msg[96];
    UWORD   state;
    UBYTE   link, keyed, want, scan_busy, scan_for_join, scan_done;
    UBYTE   ev_setssid_fail, ev_link_up, ev_link_down, ev_keyed, has_sae;
    UWORD   scan_sync;
    ULONG   scan_t0, join_t0, retry_at, info_at, backoff;
    struct fm_bss scan[SCAN_MAX];
    int     nscan;
    struct fm_bss cur;
    WORD    rssi;
    UWORD   rate;
    struct IOStdReq *scan_req[4];
    ULONG   ms, last_us, traffic_ms;

    /* prefs */
    char    ssid[33], pass[64], country[3], fwdir[96];
    ULONG   sdclock;
    UBYTE   pm, force_poll;

    /* log */
    char    log[LOGSZ];
    ULONG   logpos;
    char    line[160];
    UWORD   linelen;
};

static struct WifiBase *g_w;        /* for the process entry and wifi_log (single unit) */

/* ---- helpers --------------------------------------------------------------- */
void *memcpy(void *d, const void *s, unsigned long n)
{
    UBYTE *a = d; const UBYTE *b = s;
    if (!(((ULONG)a | (ULONG)b) & 3)) {
        ULONG *la = (ULONG *)a; const ULONG *lb = (const ULONG *)b;
        while (n >= 16) { la[0] = lb[0]; la[1] = lb[1]; la[2] = lb[2]; la[3] = lb[3]; la += 4; lb += 4; n -= 16; }
        while (n >= 4) { *la++ = *lb++; n -= 4; }
        a = (UBYTE *)la; b = (const UBYTE *)lb;
    }
    while (n--) *a++ = *b++;
    return d;
}
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, unsigned max) { unsigned i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static int mcmp(const void *a, const void *b, unsigned n) { const UBYTE *x = a, *y = b; while (n--) if (*x++ != *y++) return 1; return 0; }
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

uint32_t wifi_usecs(void)
{
    ULONG v = *(volatile ULONG *)0xF2003004u;          /* system timer CLO, 1 MHz, little-endian */
    return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
}
void wifi_udelay(uint32_t us) { uint32_t t0 = wifi_usecs(); while (wifi_usecs() - t0 < us) ; }

static ULONG now_ms(struct WifiBase *w)
{
    ULONG u = wifi_usecs(), d = u - w->last_us;
    if (d >= 1000) { w->ms += d / 1000; w->last_us = u - d % 1000; }
    return w->ms;
}

/* ---- log: a ring the tool reads, and serial lines "[alice68wifi] ..." -------- */
static void log_char(struct WifiBase *w, char c)
{
    w->log[w->logpos++ % LOGSZ] = c;
    if (w->linelen < sizeof w->line - 1) w->line[w->linelen++] = c;
    if (c == '\n') {
        static const char pre[] = "[alice68wifi] ";
        Disable();
        for (const char *p = pre; *p; p++) RawPutChar(*p);
        for (unsigned i = 0; i < w->linelen; i++) RawPutChar(w->line[i]);
        Enable();
        w->linelen = 0;
    }
}

static void fmt_num(struct WifiBase *w, ULONG v, int base, int width, char pad, int neg)
{
    char t[12]; int n = 0;
    do { t[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) t[n++] = '-';
    while (n < width) { log_char(w, pad); width--; }
    while (n) log_char(w, t[--n]);
}

void wifi_log(const char *f, ...)
{
    struct WifiBase *w = g_w;
    if (!w) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { log_char(w, *f); continue; }
        f++;
        char pad = ' '; int width = 0;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
        switch (*f) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *); while (s && *s) log_char(w, *s++); break; }
        case 'c': log_char(w, (char)__builtin_va_arg(ap, int)); break;
        case 'd': { LONG v = __builtin_va_arg(ap, LONG); fmt_num(w, v < 0 ? (ULONG)-v : (ULONG)v, 10, width, pad, v < 0); break; }
        case 'u': fmt_num(w, __builtin_va_arg(ap, ULONG), 10, width, pad, 0); break;
        case 'x': fmt_num(w, __builtin_va_arg(ap, ULONG), 16, width, pad, 0); break;
        default: log_char(w, *f); break;
        }
    }
    __builtin_va_end(ap);
}

static void say(struct WifiBase *w, const char *m)
{
    scpy(w->msg, m, sizeof w->msg);
    wifi_log("%s\n", m);
}

/* ---- control messages (ioctls and iovars) ---------------------------------- */
static void rx_all(struct WifiBase *w);

static void ctl_rx(struct WifiBase *w, const UBYTE *m, unsigned len)
{
    struct bcdc_reply r;
    if (bcdc_parse_reply(m, len, &r) || r.reqid != w->ctl_wait || w->ctl_got) return;
    unsigned n = r.len < w->ctl_outmax ? r.len : w->ctl_outmax;
    if (w->ctl_out) memcpy(w->ctl_out, r.data, n);
    w->ctl_status = r.error ? (r.status ? r.status : -1) : 0;
    w->ctl_got = 1;
}

static int wait_credit(struct WifiBase *w)
{
    ULONG t0 = wifi_usecs();
    while (!sdpcm_credit(w->txseq, w->maxseq)) {
        rx_all(w);
        if (wifi_usecs() - t0 > 1000000) return -1;
    }
    return 0;
}

/* send a command; buf holds len bytes in and receives up to outmax bytes out */
static LONG ctl(struct WifiBase *w, ULONG cmd, int set, void *buf, unsigned len, unsigned outmax)
{
    if (!w->chip_up) return -100;
    unsigned plen = set || len > outmax ? len : outmax;
    if (plen > CTLMAX) return -101;
    UBYTE *f = w->ctlbuf;
    memcpy(f + SDPCM_HDRLEN + BCDC_DCMD_HDR, buf, len);
    memset(f + SDPCM_HDRLEN + BCDC_DCMD_HDR + len, 0, plen - len);
    UWORD id = ++w->reqid;
    bcdc_dcmd(f + SDPCM_HDRLEN, cmd, set, id, 0, plen);
    if (wait_credit(w)) { wifi_log("no credit for command %u\n", cmd); return -102; }
    unsigned n = sdpcm_frame(f, SDPCM_CHAN_CONTROL, w->txseq, BCDC_DCMD_HDR + plen);
    w->txseq++;
    w->ctl_wait = id; w->ctl_got = 0; w->ctl_out = buf; w->ctl_outmax = outmax; w->ctl_status = 0;
    if (chip_frame(&w->c, f, n, 1)) { wifi_log("command %u not sent\n", cmd); return -103; }
    ULONG t0 = wifi_usecs();
    while (!w->ctl_got) {
        rx_all(w);
        if (wifi_usecs() - t0 > 2000000) { wifi_log("command %u: no answer\n", cmd); w->ctl_wait = 0; return -104; }
    }
    w->ctl_wait = 0;
    return w->ctl_status;
}

static LONG cmd_int(struct WifiBase *w, ULONG cmd, ULONG v)
{
    UBYTE b[4]; fm_put32(b, v);
    return ctl(w, cmd, 1, b, 4, 0);
}
static LONG cmd_get_int(struct WifiBase *w, ULONG cmd, ULONG *v)
{
    UBYTE b[4] = { 0, 0, 0, 0 };
    LONG e = ctl(w, cmd, 0, b, 4, 4);
    if (!e) *v = fm_le32(b);
    return e;
}
static LONG iov_set(struct WifiBase *w, const char *name, const void *val, unsigned len)
{
    unsigned n = bcdc_iovar(w->iobuf, CTLMAX, name, val, len);
    if (!n) return -105;
    return ctl(w, WLC_SET_VAR, 1, w->iobuf, n, 0);
}
static LONG iov_int(struct WifiBase *w, const char *name, ULONG v)
{
    UBYTE b[4]; fm_put32(b, v);
    return iov_set(w, name, b, 4);
}
static LONG iov_get(struct WifiBase *w, const char *name, void *out, unsigned outlen)
{
    unsigned n = bcdc_iovar(w->iobuf, CTLMAX, name, 0, 0);
    if (!n || outlen > CTLMAX) return -105;
    LONG e = ctl(w, WLC_GET_VAR, 0, w->iobuf, n, outlen);
    if (!e) memcpy(out, w->iobuf, outlen);
    return e;
}

/* ---- netdev: link state ------------------------------------------------------ */
static void link_set(struct WifiBase *w, int up)
{
    if (w->link == up) return;
    w->link = (UBYTE)up;
    if (w->attached && w->started && w->sops->nso_LinkChange) {
        struct NetDevLinkState ls;
        ls.ndls_Flags = up ? NDLF_UP | NDLF_FULL_DUPLEX : 0;
        ls.ndls_SpeedMbps = up ? (w->rate ? w->rate : 54) : 0;
        w->sops->nso_LinkChange(w->stackctx, &ls);
    }
}

/* ---- events ------------------------------------------------------------------ */
static void scan_add(void *ctx, const struct fm_bss *b)
{
    struct WifiBase *w = ctx;
    if (!b->ssid_len) return;                       /* hidden networks: not listed */
    for (int i = 0; i < w->nscan; i++)
        if (!mcmp(w->scan[i].bssid, b->bssid, 6)) { if (b->rssi > w->scan[i].rssi) w->scan[i] = *b; return; }
    if (w->nscan < SCAN_MAX) w->scan[w->nscan++] = *b;
}

static void event(struct WifiBase *w, const struct fm_event *e)
{
    switch (e->type) {
    case E_ESCAN_RESULT:
        if (e->status == E_STATUS_PARTIAL) fm_escan_parse(e->data, e->datalen, scan_add, w);
        else w->scan_done = 1;
        break;
    case E_SET_SSID:
        if (e->status != E_STATUS_SUCCESS) { w->ev_setssid_fail = 1; wifi_log("join failed (status %u)\n", e->status); }
        break;
    case E_LINK:
        if (e->flags & E_FLAG_LINK) w->ev_link_up = 1;
        else { w->ev_link_down = 1; wifi_log("link down (reason %u)\n", e->reason); }
        break;
    case E_PSK_SUP:
        if (e->status == E_STATUS_FWSUP_COMPLETED) w->ev_keyed = 1;
        else wifi_log("supplicant state %u, reason %u\n", e->status, e->reason);
        break;
    case E_DEAUTH: case E_DEAUTH_IND: case E_DISASSOC: case E_DISASSOC_IND:
        wifi_log("disconnected by the network (event %u, reason %u)\n", e->type, e->reason);
        w->ev_link_down = 1;
        break;
    default:
        break;
    }
}

/* ---- receive ---------------------------------------------------------------- */
static UBYTE *pool_get(struct WifiBase *w)
{
    UBYTE *b = 0;
    Disable();
    if (w->nfree) b = w->pool + (ULONG)w->freelist[--w->nfree] * BUFSZ;
    Enable();
    return b;
}
static void pool_put(struct WifiBase *w, UBYTE *b)
{
    Disable();
    w->freelist[w->nfree++] = (UWORD)((ULONG)(b - w->pool) / BUFSZ);
    Enable();
}
static int in_pool(struct WifiBase *w, UBYTE *b) { return w->pool && b >= w->pool && b < w->pool + w->npool * BUFSZ; }

static void rx_flush(struct WifiBase *w)
{
    if (!w->nrxd) return;
    ULONG n = w->nrxd, took = 0;
    if (w->attached && w->started) took = w->sops->nso_RxInput(w->stackctx, w->rxd, n);
    for (ULONG i = took; i < n; i++) { pool_put(w, w->rxd[i].nrd_Cookie); w->rx_drop++; }
    w->nrxd = 0;
}

/* a frame from the data/event channel at p (after SDPCM), in buffer b */
static void rx_payload(struct WifiBase *w, UBYTE *b, const UBYTE *p, unsigned len)
{
    unsigned dl;
    const UBYTE *d = bcdc_data(p, len, &dl);
    struct fm_event e;
    if (!d) { if (in_pool(w, b)) pool_put(w, b); return; }
    if (fm_event_parse(d, dl, &e)) { event(w, &e); if (in_pool(w, b)) pool_put(w, b); return; }
    if (!in_pool(w, b) || !w->attached || !w->started || dl < 14) { if (in_pool(w, b)) pool_put(w, b); w->rx_drop++; return; }
    struct NetDevRxDesc *r = &w->rxd[w->nrxd++];
    r->nrd_Data = (APTR)d;
    r->nrd_Len = dl;
    r->nrd_Flags = (d[0] & 1) ? (d[0] == 0xff && d[1] == 0xff ? NDRF_BCAST : NDRF_MCAST) : 0;
    r->nrd_CsumRaw = 0;
    r->nrd_VlanTci = 0;
    r->nrd_Cookie = b;
    w->rx_pkts++; w->rx_bytes += dl;
    w->traffic_ms = w->ms;
    if (w->nrxd >= w->rxbatch_max) rx_flush(w);
}

static void rx_fail(struct WifiBase *w)
{
    chip_w8(&w->c, 0x1000d, 1);                     /* FRAMECTRL: terminate the read */
    for (int i = 0; i < 100; i++) {
        if (!chip_r8(&w->c, 0x1001b) && !chip_r8(&w->c, 0x1001c)) break;
        wifi_udelay(10);
    }
    chip_w32(&w->c, w->c.sdio_base + SD_TOSBMAILBOX, 1);     /* NAK: send it again */
}

static void rx_glom(struct WifiBase *w, const UBYTE *desc, unsigned dlen)
{
    unsigned nsub = dlen / 2;
    UWORD sub[32];
    if (nsub > 32) nsub = 32;
    for (unsigned i = 0; i < nsub; i++) sub[i] = fm_le16(desc + 2 * i);
    for (unsigned i = 0; i < nsub; i++) {
        unsigned n = (sub[i] + 3) & ~3u;
        if (n > BUFSZ || n < SDPCM_HDRLEN) { rx_fail(w); return; }
        UBYTE *b = (w->attached && w->started) ? pool_get(w) : 0;
        UBYTE *buf = b ? b : w->rxbuf;
        if (chip_frame(&w->c, buf, n, 0)) { if (b) pool_put(w, b); return; }
        UBYTE *f = buf;
        if (i == 0) f += SDPCM_HDRLEN;              /* the superframe's own header */
        struct sdpcm_rx r;
        if (sdpcm_parse(f, &r) || r.chan == SDPCM_CHAN_CONTROL || r.chan == SDPCM_CHAN_GLOM ||
            (unsigned)(f - buf) + r.len > n) { if (b) pool_put(w, b); continue; }
        rx_payload(w, buf, f + r.dataoff, r.len - r.dataoff);
    }
}

static void rx_all(struct WifiBase *w)
{
    unsigned nextlen = 0;
    for (int guard = 0; guard < 64; guard++) {
        UBYTE *b = (w->attached && w->started) ? pool_get(w) : 0;
        UBYTE *buf = b ? b : w->rxbuf;
        unsigned first = nextlen && nextlen <= BUFSZ ? nextlen : 16;
        if (chip_frame(&w->c, buf, first, 0)) { if (b) pool_put(w, b); break; }
        struct sdpcm_rx r;
        int k = sdpcm_parse(buf, &r);
        if (k == 1) { if (b) pool_put(w, b); break; }
        if (k < 0 || (nextlen && r.len > first)) {
            wifi_log("bad frame header (%d)\n", k);
            if (b) pool_put(w, b);
            rx_fail(w);
            break;
        }
        w->maxseq = r.maxseq;
        if (r.len > BUFSZ) { if (b) pool_put(w, b); rx_fail(w); break; }
        unsigned total = (r.len + 3) & ~3u;
        if (total > first && chip_frame(&w->c, buf + first, total - first, 0)) { if (b) pool_put(w, b); break; }
        nextlen = r.nextlen;
        const UBYTE *p = buf + r.dataoff;
        unsigned plen = r.len - r.dataoff;
        switch (r.chan) {
        case SDPCM_CHAN_CONTROL:
            ctl_rx(w, p, plen);
            if (b) pool_put(w, b);
            break;
        case SDPCM_CHAN_EVENT:
        case SDPCM_CHAN_DATA:
            rx_payload(w, buf, p, plen);
            break;
        case SDPCM_CHAN_GLOM:
            if (b) pool_put(w, b);
            {
                UWORD d[32]; unsigned n = plen > sizeof d ? sizeof d : plen;
                memcpy(d, p, n);
                rx_glom(w, (const UBYTE *)d, n);
            }
            nextlen = 0;
            break;
        default:
            if (b) pool_put(w, b);
            break;
        }
    }
    rx_flush(w);
}

/* ---- transmit ---------------------------------------------------------------- */
static void tx_done_flush(struct WifiBase *w)
{
    if (!w->ndone) return;
    if (w->attached && w->sops->nso_TxDone) w->sops->nso_TxDone(w->stackctx, w->done, w->ndone);
    w->ndone = 0;
}

static void tx_all(struct WifiBase *w)
{
    while (w->tx_tail != w->tx_head) {
        if (!sdpcm_credit(w->txseq, w->maxseq)) break;
        struct txent *t = &w->tx[w->tx_tail % TXRING];
        ULONG len = 0;
        for (UWORD i = 0; i < t->nseg; i++) len += t->sg[i].nsg_Len;
        if (len < 14 || len > 1518) { w->tx_err++; }
        else {
            UBYTE *f = w->txbuf, *q = f + SDPCM_HDRLEN + BCDC_DATA_HDR;
            for (UWORD i = 0; i < t->nseg; i++) { memcpy(q, t->sg[i].nsg_Data, t->sg[i].nsg_Len); q += t->sg[i].nsg_Len; }
            unsigned n = sdpcm_frame(f, SDPCM_CHAN_DATA, w->txseq, len);
            w->txseq++;
            if (chip_frame(&w->c, f, n, 1)) w->tx_err++;
            else { w->tx_pkts++; w->tx_bytes += len; }
        }
        w->done[w->ndone++] = t->cookie;
        w->tx_tail++;
        w->traffic_ms = w->ms;
        if (w->ndone == TXRING) tx_done_flush(w);
    }
    tx_done_flush(w);
}

static void tx_drop_all(struct WifiBase *w)
{
    while (w->tx_tail != w->tx_head) {
        w->done[w->ndone++] = w->tx[w->tx_tail % TXRING].cookie;
        w->tx_tail++;
        w->tx_drop++;
        if (w->ndone == TXRING) tx_done_flush(w);
    }
    tx_done_flush(w);
}

/* ---- netdev data-path entries (called by the stack, any task) --------------- */
static LONG ndo_TxSubmit(APTR ctx, const struct NetDevTxDesc *d, ULONG count)
{
    struct WifiBase *w = ctx;
    ULONG n = 0;
    Disable();
    while (n < count && w->tx_head - w->tx_tail < TXRING) {
        if (d[n].ntd_NumSegs == 0 || d[n].ntd_NumSegs > MAXSEGS) break;
        struct txent *t = &w->tx[w->tx_head % TXRING];
        t->cookie = d[n].ntd_Cookie;
        t->nseg = d[n].ntd_NumSegs;
        for (UWORD i = 0; i < t->nseg; i++) t->sg[i] = d[n].ntd_Segs[i];
        w->tx_head++;
        n++;
    }
    Enable();
    return (LONG)n;
}
static VOID ndo_TxKick(APTR ctx) { struct WifiBase *w = ctx; if (w->task) Signal(w->task, w->sig_kick); }
static VOID ndo_RxRelease(APTR ctx, APTR cookie) { pool_put(ctx, cookie); }
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
static VOID ndo_DmaFree(APTR ctx, APTR p, ULONG size)
{
    (void)ctx; (void)size;
    if (p) FreeMem((APTR)((ULONG *)p)[-1], ((ULONG *)p)[-2]);
}
static const struct NetDevDrvOps drv_ops = { ndo_TxSubmit, ndo_TxKick, ndo_RxRelease, ndo_DmaAlloc, ndo_DmaFree };

/* ---- files -------------------------------------------------------------------- */
static UBYTE *load(struct WifiBase *w, const char *name, ULONG *len, ULONG extra)
{
    ULONG fh = DosOpen(w->dos, name, MODE_OLDFILE);
    if (!fh) return 0;
    DosSeek(w->dos, fh, 0, OFFSET_END);
    LONG n = DosSeek(w->dos, fh, 0, OFFSET_BEGINNING);
    UBYTE *b = 0;
    if (n > 0 && n < 4 * 1024 * 1024 && (b = AllocMem((ULONG)n + extra + 16, MEMF_PUBLIC | MEMF_CLEAR))) {
        if (DosRead(w->dos, fh, b, n) != n) { FreeMem(b, (ULONG)n + extra + 16); b = 0; }
        else *len = (ULONG)n;
    }
    DosClose(w->dos, fh);
    return b;
}

/* first of the names found in the firmware folder */
static UBYTE *load_any(struct WifiBase *w, const char *const *stems, const char *suffix, ULONG *len, ULONG extra, char *which)
{
    char path[160];
    for (int i = 0; stems[i]; i++) {
        unsigned n = 0;
        for (const char *p = w->fwdir; *p && n < 100; ) path[n++] = *p++;
        for (const char *p = stems[i]; *p && n < 140; ) path[n++] = *p++;
        for (const char *p = suffix; *p && n < 158; ) path[n++] = *p++;
        path[n] = 0;
        UBYTE *b = load(w, path, len, extra);
        if (b) { scpy(which, path, 160); return b; }
    }
    return 0;
}

static void prefs(struct WifiBase *w)
{
    w->ssid[0] = w->pass[0] = w->country[0] = 0;
    scpy(w->fwdir, "DEVS:Firmware/", sizeof w->fwdir);
    w->sdclock = 25000000;
    w->pm = 0;
    w->force_poll = 0;
    ULONG len = 0;
    UBYTE *b = load(w, "ENV:alice68wifi.prefs", &len, 1);
    if (!b) b = load(w, "ENVARC:alice68wifi.prefs", &len, 1);
    if (!b) { wifi_log("no ENV:alice68wifi.prefs\n"); return; }
    char *p = (char *)b, *end = p + len;
    while (p < end) {
        char *l = p;
        while (p < end && *p != '\n') p++;
        char *e = p;
        if (p < end) p++;
        while (l < e && (*l == ' ' || *l == '\t')) l++;
        while (e > l && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
        if (l == e || *l == '#' || *l == ';') continue;
        char *eq = l;
        while (eq < e && *eq != '=') eq++;
        if (eq == e) continue;
        char *k = l, *ke = eq, *v = eq + 1;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        while (v < e && (*v == ' ' || *v == '\t')) v++;
        if (e - v >= 2 && *v == '"' && e[-1] == '"') { v++; e--; }
        *ke = 0; *e = 0;
        if (sieq(k, "SSID")) scpy(w->ssid, v, sizeof w->ssid);
        else if (sieq(k, "PASSWORD") || sieq(k, "PSK") || sieq(k, "PASSPHRASE")) scpy(w->pass, v, sizeof w->pass);
        else if (sieq(k, "COUNTRY")) scpy(w->country, v, sizeof w->country);
        else if (sieq(k, "FIRMWARE")) scpy(w->fwdir, v, sizeof w->fwdir);
        else if (sieq(k, "SDCLOCK")) { ULONG n = 0; while (*v >= '0' && *v <= '9') n = n * 10 + (ULONG)(*v++ - '0'); if (n >= 400000 && n <= 50000000) w->sdclock = n; }
        else if (sieq(k, "POWERSAVE")) w->pm = (UBYTE)(*v == '1' ? 1 : *v == '2' ? 2 : 0);
        else if (sieq(k, "POLL")) w->force_poll = (UBYTE)(*v == '1' || sieq(v, "on") || sieq(v, "yes"));
        else wifi_log("prefs: unknown setting %s\n", k);
    }
    FreeMem(b, len + 1 + 16);
    wifi_log("prefs: SSID \"%s\", %s, country %s\n", w->ssid, w->pass[0] ? "password set" : "no password", w->country[0] ? w->country : "default");
}

/* ---- bring-up ----------------------------------------------------------------- */
static int clm_upload(struct WifiBase *w, const UBYTE *blob, ULONG len)
{
    UBYTE *d = w->txbuf;                    /* 12-byte header + up to 1400 bytes */
    ULONG off = 0;
    while (off < len) {
        ULONG n = len - off > 1400 ? 1400 : len - off;
        UWORD flag = 0x1000 | (off == 0 ? 2 : 0) | (off + n >= len ? 4 : 0);
        fm_put16(d, flag);
        fm_put16(d + 2, 2);                 /* type: CLM */
        fm_put32(d + 4, n);
        fm_put32(d + 8, 0);
        memcpy(d + 12, blob + off, n);
        LONG e = iov_set(w, "clmload", d, 12 + n);
        if (e) { wifi_log("CLM upload failed at %u (%d)\n", off, e); return -1; }
        off += n;
    }
    return 0;
}

static int bringup(struct WifiBase *w)
{
    char which[160];
    w->state = WS_STARTING;
    prefs(w);
    w->mbox = OpenResource("mailbox.resource");
    if (!w->mbox) { say(w, "mailbox.resource missing: needs Emu68 1.1"); return -1; }

    /* firmware names per chip; the files are read once the chip has said which it is */
    static const char *const s43455[] = { "cyfmac43455-sdio", "cyfmac43455-sdio-standard", "brcmfmac43455-sdio.raspberrypi,4-model-b", "brcmfmac43455-sdio", 0 };
    static const char *const s43456[] = { "brcmfmac43456-sdio", "brcmfmac43456-sdio.raspberrypi,4-compute-module", 0 };
    static const char *const nv43455[] = { "brcmfmac43455-sdio.raspberrypi,4-model-b", "brcmfmac43455-sdio", "cyfmac43455-sdio", 0 };
    static const char *const nv43456[] = { "brcmfmac43456-sdio.raspberrypi,4-compute-module", "brcmfmac43456-sdio", 0 };

    sdh_pins();
    MB_SetGPIOState(w->mbox, 1, 0);         /* WL_ON low: chip off */
    wifi_udelay(20000);
    MB_SetGPIOState(w->mbox, 1, 1);         /* and on */
    wifi_udelay(150000);
    MB_SetClockState(w->mbox, 1, 1);
    ULONG base = MB_GetClockRate(w->mbox, 1);
    if (!base || base == 0xffffffffu) {
        ULONG caps = *(volatile ULONG *)(SDH_BASE + 0x40);
        caps = caps >> 24 | (caps >> 8 & 0xff00) | (caps << 8 & 0xff0000) | caps << 24;
        base = ((caps >> 8) & 0xff) * 1000000u;
        if (!base) base = 250000000u;
    }
    if (sdh_init(&w->h, base)) { say(w, "the WiFi chip does not answer on SDIO"); return -2; }
    if (sdh_bus4(&w->h, w->sdclock)) { say(w, "SDIO bus setup failed"); return -3; }
    if (chip_attach(&w->c, &w->h)) { say(w, "WiFi chip not recognised"); return -4; }

    const char *const *fw = w->c.id == 0x4345 && w->c.rev == 9 ? s43456 : s43455;
    const char *const *nvn = fw == s43456 ? nv43456 : nv43455;
    if (w->c.id != 0x4345) wifi_log("chip %x is not a 43455/43456: trying the 43455 files\n", w->c.id);
    ULONG fwlen = 0, nvlen = 0, clmlen = 0;
    UBYTE *fwb = load_any(w, fw, ".bin", &fwlen, 0, which);
    if (!fwb) { say(w, fw == s43456 ? "firmware missing: copy brcmfmac43456-sdio.bin to DEVS:Firmware/" : "firmware missing: copy cyfmac43455-sdio-standard.bin to DEVS:Firmware/"); return -5; }
    wifi_log("firmware %s\n", which);
    UBYTE *nvb = load_any(w, nvn, ".txt", &nvlen, 16, which);
    if (!nvb) { FreeMem(fwb, fwlen + 16); say(w, "NVRAM missing: copy brcmfmac43455-sdio.txt to DEVS:Firmware/"); return -6; }
    wifi_log("NVRAM %s\n", which);
    UBYTE *nvimg = AllocMem(nvlen + 32, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG nvimglen = nvimg ? fm_nvram(nvb, nvlen, nvimg) : 0;
    FreeMem(nvb, nvlen + 16 + 16);
    int e = nvimg ? chip_load(&w->c, fwb, fwlen, nvimg, nvimglen) : -7;
    FreeMem(fwb, fwlen + 16);
    if (nvimg) FreeMem(nvimg, nvlen + 32);
    if (e) { say(w, "firmware did not start"); return -8; }
    w->chip_up = 1;
    w->txseq = 0xff;
    w->maxseq = 0;

    /* the firmware's setup */
    iov_int(w, "bus:txglom", 0);            /* one frame per transfer from the chip */
    ULONG ver = 0;
    if (cmd_get_int(w, WLC_GET_VERSION, &ver)) { say(w, "firmware does not answer commands"); return -9; }
    iov_get(w, "cur_etheraddr", w->mac, 6);
    memset(w->fwver, 0, sizeof w->fwver);
    iov_get(w, "ver", w->fwver, sizeof w->fwver - 1);
    for (char *p = w->fwver; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    wifi_log("MAC %02x:%02x:%02x:%02x:%02x:%02x, %s\n", w->mac[0], w->mac[1], w->mac[2], w->mac[3], w->mac[4], w->mac[5], w->fwver);
    UBYTE *clm = load_any(w, fw, ".clm_blob", &clmlen, 0, which);
    if (clm) { if (!clm_upload(w, clm, clmlen)) wifi_log("CLM %s\n", which); FreeMem(clm, clmlen + 16); }
    else wifi_log("no .clm_blob: using the firmware's built-in regulatory data\n");
    memset(w->caps, 0, sizeof w->caps);
    iov_get(w, "cap", w->caps, sizeof w->caps - 1);
    w->has_sae = 0;
    for (char *p = w->caps; *p; p++)
        if ((p == w->caps || p[-1] == ' ') && p[0] == 's' && p[1] == 'a' && p[2] == 'e' && (p[3] == ' ' || !p[3])) w->has_sae = 1;
    if (w->country[0] && w->country[1]) {
        UBYTE cc[12];
        fm_country(cc, w->country);
        if (iov_set(w, "country", cc, 12)) wifi_log("country %s not accepted\n", w->country);
    }
    UBYTE mask[EVENT_MASK_LEN];
    memset(mask, 0, sizeof mask);
    static const UBYTE evs[] = { E_SET_SSID, E_LINK, E_AUTH, E_ASSOC, E_DEAUTH, E_DEAUTH_IND, E_DISASSOC, E_DISASSOC_IND, E_PSK_SUP, E_ESCAN_RESULT, E_IF };
    for (unsigned i = 0; i < sizeof evs; i++) fm_event_mask_set(mask, evs[i]);
    iov_set(w, "event_msgs", mask, sizeof mask);
    cmd_int(w, WLC_SET_SCAN_CHANNEL_TIME, 40);
    cmd_int(w, WLC_SET_SCAN_UNASSOC_TIME, 40);
    cmd_int(w, WLC_SET_SCAN_PASSIVE_TIME, 120);
    cmd_int(w, WLC_SET_PM, w->pm);
    iov_int(w, "arp_ol", 0);
    iov_int(w, "arpoe", 0);
    iov_int(w, "ndoe", 0);
    iov_int(w, "toe", 0);
    cmd_int(w, WLC_UP, 0);
    cmd_int(w, WLC_SET_INFRA, 1);
    cmd_int(w, WLC_SET_AP, 0);
    w->state = WS_IDLE;
    say(w, w->ssid[0] ? "ready" : "ready - no network set: put SSID= in ENV:alice68wifi.prefs");
    return 0;
}

static void irq_setup(struct WifiBase *w)
{
    extern void stub_irq(void);
    w->poll = 1;
    if (w->force_poll) { wifi_log("polling (POLL=1)\n"); return; }
    w->gic = OpenLibrary("gic400.library", 0);
    if (!w->gic) { wifi_log("no gic400.library: polling\n"); return; }
    w->irq.is_Node.ln_Type = 2;
    w->irq.is_Node.ln_Name = (char *)DEVNAME;
    w->irq.is_Data = w;
    w->irq.is_Code = stub_irq;
    if (AddIntServerEx(w->gic, GIC_IRQ, 0, 0, &w->irq)) { wifi_log("interrupt %u taken: polling\n", GIC_IRQ); return; }
    w->irq_on = 1;
    w->poll = 0;
    sdh_card_irq(&w->h, 1);
    wifi_log("card interrupt on GIC %u\n", GIC_IRQ);
}

void dev_irq(struct WifiBase *w)
{
    if (sdh_card_irq_pending(&w->h)) {
        sdh_card_irq(&w->h, 0);             /* off until the process has served the chip */
        w->irqs++;
        w->spurious = 0;
        Signal(w->task, w->sig_irq);
    } else if (++w->spurious > 5000) {      /* someone else's level interrupt we cannot clear */
        DisableIntGic(w->gic, GIC_IRQ);
        w->poll = 1;
        Signal(w->task, w->sig_irq);
    }
}

/* ---- connecting ------------------------------------------------------------------ */
static void scan_start(struct WifiBase *w)
{
    UBYTE p[80];
    w->nscan = 0;
    w->scan_done = 0;
    w->scan_t0 = now_ms(w);
    unsigned n = fm_escan_params(p, ++w->scan_sync);
    LONG e = iov_set(w, "escan", p, n);
    if (e) { wifi_log("scan refused (%d)\n", e); w->scan_done = 1; }
    w->scan_busy = 1;
}

static void retry(struct WifiBase *w, const char *why)
{
    say(w, why);
    link_set(w, 0);
    w->state = WS_RETRY;
    w->backoff = w->backoff ? (w->backoff * 2 > 30000 ? 30000 : w->backoff * 2) : 2000;
    w->retry_at = now_ms(w) + w->backoff;
}

static void join(struct WifiBase *w, const struct fm_bss *b)
{
    struct fm_bss any;
    if (!b) {                               /* not seen in the scan: hidden, or out of range - try anyway */
        memset(&any, 0, sizeof any);
        any.security = w->pass[0] ? SEC_WPA2 : SEC_OPEN;
        any.group_cipher = any.pairwise = 4;
        b = &any;
    }
    w->cur = *b;
    int open = !w->pass[0];
    int sae = !open && (b->security & SEC_WPA3) && !(b->security & (SEC_WPA2 | SEC_WPA)) && w->has_sae;
    if (!open && (b->security & (SEC_OPEN | SEC_WEP)) && !(b->security & (SEC_WPA | SEC_WPA2 | SEC_WPA3))) {
        if (b->security & SEC_WEP) { retry(w, "the network uses WEP, which is not supported"); return; }
        open = 1;
    }
    if (!open && (b->security & SEC_WPA3) && !(b->security & (SEC_WPA2 | SEC_WPA)) && !w->has_sae) {
        retry(w, "the network is WPA3-only and this firmware has no SAE");
        return;
    }
    if (!open && (b->security & SEC_8021X) && !(b->security & (SEC_WPA | SEC_WPA2 | SEC_WPA3))) {
        retry(w, "the network is WPA-Enterprise, which is not supported");
        return;
    }
    ULONG wsec = 0, auth = 0;
    if (!open) {
        if (b->pairwise == 4 || b->group_cipher == 4) wsec |= WSEC_AES;
        if (b->pairwise == 2 || b->group_cipher == 2) wsec |= WSEC_TKIP;
        if (!wsec) wsec = WSEC_AES;
        auth = sae ? WPA3_AUTH_SAE_PSK : (b->security & (SEC_WPA2 | SEC_WPA3)) ? WPA2_AUTH_PSK : WPA_AUTH_PSK;
    }
    cmd_int(w, WLC_DISASSOC, 0);
    iov_int(w, "wsec", wsec);
    iov_int(w, "wpa_auth", auth);
    iov_int(w, "auth", sae ? 3 : 0);
    iov_int(w, "mfp", sae ? MFP_REQUIRED : MFP_NONE);
    if (!open) {
        UBYTE ie[32];
        unsigned n = sae ? 0 : fm_build_wpaie(ie, b, 0);
        iov_set(w, "wpaie", ie, n);
        if (iov_int(w, "sup_wpa", 1)) { retry(w, "this firmware has no WPA supplicant"); return; }
        UBYTE k[140];
        unsigned pl = slen(w->pass);
        if (sae) {
            n = fm_sae_password(k, w->pass, pl);
            if (iov_set(w, "sae_password", k, n)) { retry(w, "SAE password not accepted"); return; }
        } else if (pl < 8 || pl > 64) {
            retry(w, "the password must have 8 to 63 characters (or 64 hex digits)");
            return;
        } else {
            n = fm_pmk(k, w->pass, pl, 0);
            if (ctl(w, WLC_SET_WSEC_PMK, 1, k, n, 0)) {
                n = fm_pmk(k, w->pass, pl, 1);
                if (ctl(w, WLC_SET_WSEC_PMK, 1, k, n, 0)) { retry(w, "the firmware refused the password"); return; }
            }
        }
    } else
        iov_int(w, "sup_wpa", 0);
    UBYTE jp[80];
    unsigned sl = slen(w->ssid);
    int known = b != &any;
    unsigned n = fm_join_params(jp, w->ssid, sl, known ? b->bssid : 0, known ? b->chanspec : 0);
    if (iov_set(w, "join", jp, n)) {
        n = fm_ssid_params(jp, w->ssid, sl, known ? b->bssid : 0);
        if (ctl(w, WLC_SET_SSID, 1, jp, n, 0)) { retry(w, "join refused by the firmware"); return; }
    }
    w->keyed = open;
    w->ev_setssid_fail = w->ev_link_up = w->ev_link_down = w->ev_keyed = 0;
    w->join_t0 = now_ms(w);
    w->state = WS_JOINING;
    wifi_log("joining \"%s\" on channel %u (%s)\n", w->ssid, b->channel,
             open ? "open" : sae ? "WPA3-SAE" : (b->security & (SEC_WPA2 | SEC_WPA3)) ? "WPA2-PSK" : "WPA-PSK");
}

static void connect_info(struct WifiBase *w)
{
    ULONG v;
    if (!cmd_get_int(w, WLC_GET_RSSI, &v)) w->rssi = (WORD)(LONG)v;
    if (!cmd_get_int(w, WLC_GET_RATE, &v)) w->rate = (UWORD)(v / 2);
    UBYTE b[8];
    if (!ctl(w, WLC_GET_BSSID, 0, b, 6, 6)) memcpy(w->cur.bssid, b, 6);
}

static void scan_reply(struct WifiBase *w)
{
    for (int i = 0; i < 4; i++) {
        Forbid();
        struct IOStdReq *io = w->scan_req[i];
        w->scan_req[i] = 0;
        Permit();
        if (!io) continue;
        struct WifiScanEntry *e = io->io_Data;
        ULONG max = io->io_Length / sizeof *e, n = 0;
        for (int k = 0; k < w->nscan && n < max; k++, n++) {
            memcpy(e[n].we_Bssid, w->scan[k].bssid, 6);
            scpy(e[n].we_Ssid, w->scan[k].ssid, sizeof e[n].we_Ssid);
            e[n].we_Rssi = w->scan[k].rssi;
            e[n].we_Channel = w->scan[k].channel;
            e[n].we_Security = w->scan[k].security;
        }
        io->io_Actual = n;
        io->io.io_Error = 0;
        ReplyMsg(io);
    }
}

static void conn_tick(struct WifiBase *w)
{
    ULONG now = now_ms(w);
    if (!w->chip_up) return;
    if (w->scan_busy && (w->scan_done || now - w->scan_t0 > 10000)) {
        w->scan_busy = 0;
        wifi_log("scan: %d networks\n", w->nscan);
        scan_reply(w);
        if (w->scan_for_join) {
            w->scan_for_join = 0;
            const struct fm_bss *best = 0;
            for (int i = 0; i < w->nscan; i++)
                if (w->scan[i].ssid_len == slen(w->ssid) && !mcmp(w->scan[i].ssid, w->ssid, w->scan[i].ssid_len) &&
                    (!best || w->scan[i].rssi > best->rssi))
                    best = &w->scan[i];
            if (!best) wifi_log("\"%s\" not seen in the scan\n", w->ssid);
            join(w, best);
        }
    }
    switch (w->state) {
    case WS_IDLE:
        if (w->want && w->ssid[0] && !w->scan_busy) { w->state = WS_SCANNING; w->scan_for_join = 1; scan_start(w); }
        break;
    case WS_RETRY:
        if ((LONG)(now - w->retry_at) >= 0 && !w->scan_busy) { w->state = WS_SCANNING; w->scan_for_join = 1; scan_start(w); }
        break;
    case WS_JOINING:
        if (w->ev_setssid_fail) { retry(w, "could not join the network"); break; }
        if (w->ev_link_down && now - w->join_t0 < 1000) w->ev_link_down = 0;   /* the old link going, from our DISASSOC */
        if (w->ev_link_down) { retry(w, w->cur.security & (SEC_WPA2 | SEC_WPA3 | SEC_WPA) ? "the network refused us: is the password right?" : "the network refused us"); break; }
        if (w->ev_keyed) w->keyed = 1;
        if (w->ev_link_up && w->keyed) {
            w->state = WS_CONNECTED;
            w->backoff = 0;
            connect_info(w);
            w->info_at = now + 5000;
            link_set(w, 1);
            char m[96];
            unsigned i = 0;
            for (const char *p = "connected to "; *p; ) m[i++] = *p++;
            for (const char *p = w->ssid; *p && i < 90; ) m[i++] = *p++;
            m[i] = 0;
            say(w, m);
            wifi_log("channel %u, %d dBm, %u Mbit/s\n", w->cur.channel, w->rssi, w->rate);
        } else if (now - w->join_t0 > 15000)
            retry(w, w->ev_link_up ? "no WPA handshake: is the password right?" : "the network did not answer");
        break;
    case WS_CONNECTED:
        if (w->ev_link_down) { w->ev_link_down = 0; retry(w, "connection lost"); break; }
        if ((LONG)(now - w->info_at) >= 0) {
            w->info_at = now + 5000;
            UWORD old = w->rate;
            connect_info(w);
            if (old != w->rate && w->attached && w->started && w->sops->nso_LinkChange) {
                struct NetDevLinkState ls = { NDLF_UP | NDLF_FULL_DUPLEX, w->rate ? w->rate : 54 };
                w->sops->nso_LinkChange(w->stackctx, &ls);
            }
        }
        break;
    default:
        break;
    }
}

/* ---- commands ---------------------------------------------------------------- */
static const UWORD cmds[] = {
    NSCMD_DEVICEQUERY, NETDEV_CMD_ATTACH, NETDEV_CMD_DETACH, NETDEV_CMD_START, NETDEV_CMD_STOP,
    NETDEV_CMD_SET_RXFILTER, NETDEV_CMD_SET_COALESCE, NETDEV_CMD_GET_STATS, NETDEV_CMD_GET_LINK, NETDEV_CMD_SET_MAC,
    WIFI_CMD_STATUS, WIFI_CMD_SCAN, WIFI_CMD_LOG, WIFI_CMD_RECONNECT, WIFI_CMD_DISCONNECT, WIFI_CMD_OFF, 0
};

static void pool_free(struct WifiBase *w)
{
    if (w->pool) FreeMem(w->pool, w->poolsize);
    w->pool = 0; w->freelist = 0; w->npool = w->nfree = 0;
}

static int do_attach(struct WifiBase *w, struct IOStdReq *io)
{
    struct NetDevAttach *a = io->io_Data;
    if (!a || io->io_Length < sizeof *a) return NDERR_BADPARAMS;
    if (w->attached) return NDERR_BUSY;
    if (!w->chip_up) { wifi_log("netdev attach: the chip is not running (%s)\n", w->msg); return IOERR_OPENFAIL; }
    if (a->nda_AbiVersion < 1) return NDERR_BADVERSION;
    ULONG n = a->nda_RxHoldReq ? a->nda_RxHoldReq + 32u : 96u;
    if (n > 256) n = 256;
    if (n < 48) n = 48;
    w->poolsize = n * BUFSZ + n * 2;
    w->pool = AllocMem(w->poolsize, MEMF_PUBLIC | MEMF_FAST);
    if (!w->pool) { w->pool = AllocMem(w->poolsize, MEMF_PUBLIC); }
    if (!w->pool) return NDERR_NOMEM;
    w->npool = n;
    w->freelist = (UWORD *)(w->pool + n * BUFSZ);
    for (ULONG i = 0; i < n; i++) w->freelist[i] = (UWORD)i;
    w->nfree = n;
    w->rxbatch_max = a->nda_RxBatch && a->nda_RxBatch < RXBATCH ? a->nda_RxBatch : RXBATCH;
    w->stackctx = a->nda_StackCtx;
    w->sops = a->nda_StackOps;
    a->nda_AbiVersion = 1;
    a->nda_DrvCtx = w;
    a->nda_DrvOps = &drv_ops;
    struct NetDevCaps *c = &a->nda_Caps;
    memset(c, 0, sizeof *c);
    c->ndc_AbiVersion = NETDEV_ABI_VERSION;
    c->ndc_Mtu = 1500;
    memcpy(c->ndc_Mac, w->mac, 6);
    c->ndc_TxMaxSegs = MAXSEGS;
    c->ndc_Features = NDCF_LINK_EVENTS | NDCF_MCAST_FILTER;
    c->ndc_TxRingSlots = TXRING;
    c->ndc_TxAlign = 0;
    c->ndc_TxInFlightMax = TXRING;
    c->ndc_RxPoolBufs = n;
    memset(&w->st, 0, sizeof w->st);
    w->tx_head = w->tx_tail = 0;
    w->ndone = w->nrxd = 0;
    w->attached = 1;
    w->started = 0;
    wifi_log("netdev: attached, %u receive buffers\n", n);
    return 0;
}

static void fill_status(struct WifiBase *w, struct WifiStatus *s)
{
    memset(s, 0, sizeof *s);
    s->ws_Version = WIFI_API_VERSION;
    s->ws_State = w->state;
    s->ws_Chip = w->c.id; s->ws_ChipRev = w->c.rev;
    memcpy(s->ws_Mac, w->mac, 6);
    if (w->state == WS_CONNECTED || w->state == WS_JOINING) {
        memcpy(s->ws_Bssid, w->cur.bssid, 6);
        scpy(s->ws_Ssid, w->ssid, sizeof s->ws_Ssid);
        s->ws_Rssi = w->rssi;
        s->ws_Channel = w->cur.channel;
        s->ws_RateMbps = w->rate;
        s->ws_Security = w->cur.security;
    } else
        scpy(s->ws_Ssid, w->ssid, sizeof s->ws_Ssid);
    s->ws_Interrupts = !w->poll;
    s->ws_SdMhz = (UWORD)(w->h.sd_hz / 1000000);
    s->ws_Attached = w->attached;
    s->ws_RxPackets = w->rx_pkts; s->ws_TxPackets = w->tx_pkts;
    s->ws_RxBytes = w->rx_bytes; s->ws_TxBytes = w->tx_bytes;
    s->ws_RxDropped = w->rx_drop; s->ws_TxErrors = w->tx_err;
    s->ws_SdErrors = w->h.errors; s->ws_Irqs = w->irqs;
    scpy(s->ws_FwVersion, w->fwver, sizeof s->ws_FwVersion);
    scpy(s->ws_Message, w->msg, sizeof s->ws_Message);
}

/* returns 1 if the request is kept (answered later) */
static int command(struct WifiBase *w, struct IOStdReq *io)
{
    LONG err = 0;
    switch (io->io.io_Command) {
    case NETDEV_CMD_ATTACH: err = do_attach(w, io); break;
    case NETDEV_CMD_DETACH:
        if (!w->attached) { err = NDERR_NOTATTACHED; break; }
        tx_drop_all(w);
        w->attached = w->started = 0;
        if (w->nfree != w->npool) wifi_log("netdev: detached with %u buffers still out\n", w->npool - w->nfree);
        pool_free(w);
        wifi_log("netdev: detached\n");
        break;
    case NETDEV_CMD_START:
        if (!w->attached) { err = NDERR_NOTATTACHED; break; }
        w->started = 1;
        w->want = 1;
        if (w->sops->nso_LinkChange) {
            struct NetDevLinkState ls = { w->link ? NDLF_UP | NDLF_FULL_DUPLEX : 0, w->link ? (w->rate ? w->rate : 54) : 0 };
            w->sops->nso_LinkChange(w->stackctx, &ls);
        }
        break;
    case NETDEV_CMD_STOP:
        if (!w->attached) { err = NDERR_NOTATTACHED; break; }
        rx_flush(w);
        w->started = 0;
        tx_drop_all(w);
        break;
    case NETDEV_CMD_SET_RXFILTER: {
        struct NetDevRxFilter *f = io->io_Data;
        if (!f) { err = NDERR_BADPARAMS; break; }
        int all = (f->ndrx_Flags & (NDFF_PROMISC | NDFF_ALLMULTI)) || f->ndrx_NumMcast > 16;
        iov_int(w, "allmulti", all);
        UBYTE b[4 + 16 * 6];
        unsigned n = all ? 0 : f->ndrx_NumMcast;
        fm_put32(b, n);
        for (unsigned i = 0; i < n; i++) memcpy(b + 4 + 6 * i, f->ndrx_McastList[i], 6);
        iov_set(w, "mcast_list", b, 4 + 6 * n);
        break;
    }
    case NETDEV_CMD_SET_COALESCE: break;
    case NETDEV_CMD_GET_STATS: {
        struct NetDevStats *s = io->io_Data;
        if (!s || io->io_Length < sizeof *s) { err = NDERR_BADPARAMS; break; }
        memset(s, 0, sizeof *s);
        s->nds_RxPackets.ndu_Lo = w->rx_pkts; s->nds_RxBytes.ndu_Lo = w->rx_bytes;
        s->nds_RxDropped.ndu_Lo = w->rx_drop; s->nds_TxPackets.ndu_Lo = w->tx_pkts;
        s->nds_TxBytes.ndu_Lo = w->tx_bytes; s->nds_TxErrors.ndu_Lo = w->tx_err;
        s->nds_TxDropped.ndu_Lo = w->tx_drop; s->nds_RxErrors.ndu_Lo = w->h.errors;
        break;
    }
    case NETDEV_CMD_GET_LINK: {
        struct NetDevLinkState *l = io->io_Data;
        if (!l) { err = NDERR_BADPARAMS; break; }
        l->ndls_Flags = w->link ? NDLF_UP | NDLF_FULL_DUPLEX : 0;
        l->ndls_SpeedMbps = w->link ? (w->rate ? w->rate : 54) : 0;
        break;
    }
    case NETDEV_CMD_SET_MAC:
        if (!io->io_Data || iov_set(w, "cur_etheraddr", io->io_Data, 6)) err = NDERR_BADPARAMS;
        else memcpy(w->mac, io->io_Data, 6);
        break;
    case WIFI_CMD_STATUS:
        if (!io->io_Data || io->io_Length < sizeof(struct WifiStatus)) { err = NDERR_BADPARAMS; break; }
        fill_status(w, io->io_Data);
        io->io_Actual = sizeof(struct WifiStatus);
        break;
    case WIFI_CMD_SCAN:
        if (!w->chip_up) { err = IOERR_OPENFAIL; break; }
        for (int i = 0; i < 4; i++)
            if (!w->scan_req[i]) {
                w->scan_req[i] = io;
                if (!w->scan_busy) scan_start(w);
                return 1;
            }
        err = NDERR_BUSY;
        break;
    case WIFI_CMD_LOG: {
        char *d = io->io_Data;
        ULONG max = io->io_Length, have = w->logpos < LOGSZ ? w->logpos : LOGSZ;
        if (have > max) have = max;
        for (ULONG i = 0; i < have; i++) d[i] = w->log[(w->logpos - have + i) % LOGSZ];
        io->io_Actual = have;
        break;
    }
    case WIFI_CMD_RECONNECT:
        prefs(w);
        if (w->state == WS_CONNECTED || w->state == WS_JOINING) cmd_int(w, WLC_DISASSOC, 0);
        link_set(w, 0);
        w->want = 1;
        w->backoff = 0;
        w->state = w->chip_up ? WS_IDLE : w->state;
        break;
    case WIFI_CMD_DISCONNECT:
        w->want = 0;
        if (w->chip_up) cmd_int(w, WLC_DISASSOC, 0);
        link_set(w, 0);
        if (w->chip_up) w->state = WS_IDLE;
        say(w, "disconnected");
        break;
    case WIFI_CMD_OFF:
        say(w, "switched off");
        w->quit = 1;
        break;
    default:
        err = IOERR_NOCMD;
    }
    io->io.io_Error = (BYTE)err;
    return 0;
}

/* ---- the process --------------------------------------------------------------- */
static void service(struct WifiBase *w)
{
    if (!w->chip_up) return;
    uint32_t is, mb;
    chip_irq_ack(&w->c, &is, &mb);
    if (mb & HMB_DATA_FWHALT) {
        say(w, "the WiFi firmware stopped");
        w->chip_up = 0;
        w->state = WS_FAILED;
        link_set(w, 0);
        return;
    }
    rx_all(w);
    tx_all(w);
}

static void wifi_proc(void)
{
    struct WifiBase *w = g_w;
    w->task = FindTask(0);
    LONG sb_port = AllocSignal(-1), sb_irq = AllocSignal(-1), sb_kick = AllocSignal(-1);
    w->sig_port = 1u << sb_port; w->sig_irq = 1u << sb_irq; w->sig_kick = 1u << sb_kick;
    struct MsgPort *tp = CreateMsgPort();
    struct timerequest *tr = tp ? CreateIORequest(tp, sizeof *tr) : 0;
    int timer_ok = tr && !OpenDevice("timer.device", 0, tr, 0);
    w->dos = OpenLibrary("dos.library", 37);
    w->rxbuf = AllocMem(4 * BUFSZ, MEMF_PUBLIC | MEMF_CLEAR);
    /* the unit's port starts taking requests now */
    Forbid();
    w->unit.unit_MsgPort.mp_SigBit = (UBYTE)sb_port;
    w->unit.unit_MsgPort.mp_SigTask = w->task;
    w->unit.unit_MsgPort.mp_Flags = PA_SIGNAL;
    w->running = 1;
    Signal(w->waiter, SIGF_SINGLE);
    w->waiter = 0;
    Permit();

    if (!timer_ok || !w->dos || !w->rxbuf || sb_kick < 0) say(w, "out of resources");
    else {
        w->txbuf = w->rxbuf + BUFSZ; w->ctlbuf = w->txbuf + BUFSZ; w->iobuf = w->ctlbuf + BUFSZ;
        w->c.bounce = AllocMem(CHIP_BOUNCE, MEMF_PUBLIC);
        w->last_us = wifi_usecs();
        wifi_log("%s", IDSTRING);
        if (!w->c.bounce || bringup(w)) { w->state = WS_FAILED; w->chip_up = 0; }
        else { irq_setup(w); w->want = 1; }
    }

    ULONG tick_mask = timer_ok ? 1u << tp->mp_SigBit : 0;
    int timer_out = 0;
    while (!w->quit) {
        if (timer_ok && !timer_out) {
            ULONG now = now_ms(w);
            int busy = now - w->traffic_ms < 300 || w->state == WS_JOINING || w->state == WS_SCANNING || w->scan_busy;
            tr->io.io_Command = TR_ADDREQUEST;
            tr->tv_secs = 0;
            tr->tv_micro = w->poll ? (busy ? 2000 : 15000) : 50000;
            SendIO(tr);
            timer_out = 1;
        }
        ULONG s = Wait(w->sig_port | w->sig_irq | w->sig_kick | tick_mask | SIGBREAKF_CTRL_C);
        if (timer_out && CheckIO(tr)) { WaitIO(tr); timer_out = 0; }
        struct IOStdReq *io;
        while ((io = GetMsg(&w->unit.unit_MsgPort)))
            if (!command(w, io)) ReplyMsg(io);
        if (w->chip_up) {
            service(w);
            if ((s & w->sig_irq) && w->irq_on && !w->poll) sdh_card_irq(&w->h, 1);
            conn_tick(w);
        }
    }

    /* shut down: chip off, requests answered */
    if (timer_out) { AbortIO(tr); WaitIO(tr); }
    if (w->irq_on) { sdh_card_irq(&w->h, 0); RemIntServerEx(w->gic, GIC_IRQ, &w->irq); w->irq_on = 0; }
    if (w->chip_up) { cmd_int(w, WLC_DOWN, 0); w->chip_up = 0; }
    if (w->mbox) MB_SetGPIOState(w->mbox, 1, 0);
    for (int i = 0; i < 4; i++) if (w->scan_req[i]) { w->scan_req[i]->io.io_Error = IOERR_ABORTED; ReplyMsg(w->scan_req[i]); w->scan_req[i] = 0; }
    pool_free(w);
    if (w->c.bounce) FreeMem(w->c.bounce, CHIP_BOUNCE);
    if (w->rxbuf) FreeMem(w->rxbuf, 4 * BUFSZ);
    w->c.bounce = 0; w->rxbuf = 0;
    if (w->gic) CloseLibrary(w->gic);
    if (timer_ok) CloseDevice(tr);
    if (tr) DeleteIORequest(tr);
    if (tp) DeleteMsgPort(tp);
    Forbid();
    w->unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    w->running = 0;
    w->task = 0;
    if (w->waiter) Signal(w->waiter, SIGF_SINGLE);
    if (w->dos) CloseLibrary(w->dos);
    /* exits under Forbid: nothing of this process runs after the closer wakes */
}

/* ---- the device ------------------------------------------------------------------ */
ULONG dev_init(struct WifiBase *w, APTR seglist)
{
    w->seglist = seglist;
    w->lib.lib_Node.ln_Type = NT_DEVICE;
    w->lib.lib_Node.ln_Name = (char *)DEVNAME;
    w->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    w->lib.lib_Version = VERSION;
    w->lib.lib_Revision = REVISION;
    w->lib.lib_IdString = (APTR)IDSTRING;
    w->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
    w->unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    NewList(&w->unit.unit_MsgPort.mp_MsgList);
    g_w = w;
    return (ULONG)w;
}

LONG dev_open(struct WifiBase *w, struct IOStdReq *io, ULONG unit, ULONG flags)
{
    (void)flags;
    io->io.io_Error = IOERR_OPENFAIL;
    if (unit != 0) return -1;
    w->lib.lib_OpenCnt++;                   /* no expunge while we wait */
    if (!w->running) {
        APTR dos = OpenLibrary("dos.library", 37);
        if (dos) {
            static const char pname[] = "alice68wifi";
            struct TagItem t[] = { { NP_Entry, (ULONG)wifi_proc }, { NP_Name, (ULONG)pname },
                                   { NP_StackSize, 16384 }, { NP_Priority, 5 }, { TAG_DONE, 0 } };
            w->waiter = FindTask(0);
            w->quit = 0;
            SetSignal(0, SIGF_SINGLE);
            if (CreateNewProc(dos, t)) Wait(SIGF_SINGLE);
            CloseLibrary(dos);
        }
    }
    if (!w->running) { w->lib.lib_OpenCnt--; return -1; }
    io->io.io_Unit = &w->unit;
    io->io.io_Error = 0;
    w->unit.unit_OpenCnt++;
    w->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return 0;
}

static ULONG do_expunge(struct WifiBase *w)
{
    if (w->lib.lib_OpenCnt || w->running) { w->lib.lib_Flags |= LIBF_DELEXP; return 0; }
    ULONG seg = (ULONG)w->seglist;
    Remove(&w->lib.lib_Node);
    g_w = 0;
    FreeMem((UBYTE *)w - w->lib.lib_NegSize, (ULONG)w->lib.lib_NegSize + w->lib.lib_PosSize);
    return seg;
}

ULONG dev_close(struct WifiBase *w, struct IOStdReq *io)
{
    io->io.io_Unit = (struct Unit *)-1;
    io->io.io_Device = (APTR)-1;
    if (w->unit.unit_OpenCnt) w->unit.unit_OpenCnt--;
    if (w->lib.lib_OpenCnt) w->lib.lib_OpenCnt--;
    /* The chip stays on after the last close (a tool run before the TCP/IP stack
       must not drop the connection); Alice68-WiFi OFF stops the process, and
       the last close then waits for it to end. */
    if (!w->lib.lib_OpenCnt && w->running && w->quit) {
        Forbid();
        if (w->running) {
            w->waiter = FindTask(0);
            SetSignal(0, SIGF_SINGLE);
            Signal(w->task, SIGBREAKF_CTRL_C);
            while (w->running) Wait(SIGF_SINGLE);
        }
        Permit();
    }
    if (!w->lib.lib_OpenCnt && (w->lib.lib_Flags & LIBF_DELEXP)) return do_expunge(w);
    return 0;
}

ULONG dev_expunge(struct WifiBase *w) { return do_expunge(w); }

void dev_beginio(struct WifiBase *w, struct IOStdReq *io)
{
    io->io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->io.io_Error = 0;
    if (io->io.io_Command == NSCMD_DEVICEQUERY) {
        struct NSDeviceQueryResult *q = io->io_Data;
        if (!q || io->io_Length < sizeof *q) io->io.io_Error = IOERR_NOCMD;
        else {
            q->DevQueryFormat = 0;
            q->SizeAvailable = sizeof *q;
            q->DeviceType = 0;              /* NSDEVTYPE_UNKNOWN, as genet.device 4.x */
            q->DeviceSubType = 0;
            q->SupportedCommands = cmds;
            io->io_Actual = sizeof *q;
        }
        if (!(io->io.io_Flags & IOF_QUICK)) ReplyMsg(io);
        return;
    }
    io->io.io_Flags &= (UBYTE)~IOF_QUICK;
    PutMsg(&w->unit.unit_MsgPort, io);
}

ULONG dev_abortio(struct WifiBase *w, struct IOStdReq *io)
{
    Forbid();
    for (struct Node *n = w->unit.unit_MsgPort.mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n == (struct Node *)io) {
            Remove(n);
            io->io.io_Error = IOERR_ABORTED;
            ReplyMsg(io);
            Permit();
            return 0;
        }
    for (int i = 0; i < 4; i++)
        if (w->scan_req[i] == io) {
            w->scan_req[i] = 0;
            io->io.io_Error = IOERR_ABORTED;
            ReplyMsg(io);
            break;
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
const ULONG dev_inittab[4] = { sizeof(struct WifiBase), (ULONG)dev_functab, 0, (ULONG)stub_init };
