/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 USB pass-through: the devices the kernel does not keep for itself
 * (everything but boot keyboards and mice), handed to Neptune in AmigaOS
 * through alice68-usb.device. Included by frf_xhci.c (it shares its rings,
 * commands and event routing).
 *
 * Each such device becomes a port of the root hub the Amiga driver
 * emulates. The Amiga posts transfers into the mailbox board
 * (alice68_usbpt.h); this side carries them out on the xHCI, CPU1, polled:
 *   control  - synchronous, on EP0. SET_CONFIGURATION / SET_INTERFACE also
 *              build the endpoint contexts, CLEAR_FEATURE(HALT) resets the
 *              endpoint (and its data toggle), SuperSpeed descriptors are
 *              shown as USB 2.0 ones (Neptune's classic interface knows no
 *              SuperSpeed);
 *   bulk/int - one request per endpoint at a time, in submission order,
 *              through a 32 KB bounce buffer per mailbox slot (the VL805
 *              reaches only the DMA pool), in chunks; AbortIO and the NAK
 *              timeout stop the endpoint and move its dequeue pointer on.
 * A completion raises the Amiga's level-2 (PORTS) interrupt.
 * A 68k RESET makes Kickstart configure the board again: that starts a new
 * session (pt_board_map -> pt_new_session on CPU1).
 */
#include "alice68_usbpt.h"

#define UHIOERR_HOSTERROR   3
#define UHIOERR_STALL       4
#define UHIOERR_TIMEOUT     6
#define UHIOERR_RUNTPACKET  9
#define UHIOERR_NAKTIMEOUT  10
#define UHIOERR_BADPARAMS   11
#define UHIOERR_BABBLE      13
#define UHIOERR_XACTERROR   15
#define IOERR_ABORTED       (-2)
#define UHFF_NOSHORTPKT     (1u << 2)
#define UHFF_NAKTIMEOUT     (1u << 3)
#define UHFF_ALLOWRUNTPKTS  (1u << 4)

/* pool: slot areas end at P_SLOTS + 33 * 16 KB = 0xC4000 */
#define P_PTRING   0x0C4000u      /* PT_NRING rings of 1 KB */
#define PT_NRING   240
#define P_PTBOUNCE 0x100000u      /* PT_REQS * 32 KB */

static volatile struct pt_hdr *pt;          /* the board, once Kickstart has configured it */
static volatile int pt_session_req;         /* CPU0 (autoconfig) asks CPU1 for a fresh session */
static int pt_port_slot[PT_PORTS + 1];
static uint32_t pt_ring_used[(PT_NRING + 31) / 32];

struct pt_ep {
    int on, mps, type, iface, ring, busy;
    int flush;                               /* failed: clean before its next transfer (main loop) */
    uint32_t c0, c1, c4;                     /* the endpoint context as configured (for drop+add) */
    struct ring r;
};
struct pt_dev {
    int port, ncfg, numcfg, cfgval;
    uint8_t alt[32];
    struct pt_ep ep[32];                     /* by DCI */
};
static struct pt_dev ptd[MAX_SLOTS + 1];
static volatile uint8_t *pt_cfg_of(int slot) { return (volatile uint8_t *)(pool + P_SLOTS + (uint32_t)slot * 0x4000u + 0x2400u); }   /* 2 KB */

struct pt_k { int slot, dci, off, chunk, zlp, fast; uint32_t trb; uint64_t deadline; };
static struct pt_k ptk[PT_REQS];

static volatile struct pt_req *pt_req(int i) { return (volatile struct pt_req *)((uintptr_t)pt + PT_OFF_REQS + (uint32_t)i * 64u); }
static uintptr_t pt_bounce(int i) { return pool + P_PTBOUNCE + (uint32_t)i * PT_MAXCHUNK; }

/* The bounce buffers (the pool's second MB) are mapped cacheable (start.c):
   the copies to and from AmigaOS buffers run at cache speed, and the VL805,
   which does not snoop, gets cache maintenance around each transfer instead.
   Before DMA: clean + invalidate (no dirty line can land on top of what the
   device writes); after an IN: invalidate again (lines fetched meanwhile). */
static void pt_dc(uintptr_t a, uint32_t n)
{
    if (!n) return;
    uintptr_t e = a + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (a &= ~(uintptr_t)63; a < e; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* copy between an AmigaOS buffer and a bounce buffer. fast: both are
   normal memory (unaligned 64-bit accesses fine); else bytes, for a buffer
   in device memory */
typedef uint64_t pt_u64u __attribute__((aligned(1), may_alias));
static void pt_copy(uintptr_t d, uintptr_t s, uint32_t n, int fast)
{
    if (fast) {
        while (n && (d & 7)) { *(volatile uint8_t *)d++ = *(volatile uint8_t *)s++; n--; }
        while (n >= 32) {
            uint64_t a = *(const pt_u64u *)s, b = *(const pt_u64u *)(s + 8), c = *(const pt_u64u *)(s + 16), e = *(const pt_u64u *)(s + 24);
            ((uint64_t *)d)[0] = a; ((uint64_t *)d)[1] = b; ((uint64_t *)d)[2] = c; ((uint64_t *)d)[3] = e;
            d += 32; s += 32; n -= 32;
        }
        while (n >= 8) { *(uint64_t *)d = *(const pt_u64u *)s; d += 8; s += 8; n -= 8; }
    }
    while (n--) *(volatile uint8_t *)d++ = *(volatile uint8_t *)s++;
    __asm__ volatile("" ::: "memory");
}

/* the board is uncached: no exclusives on it (they may never succeed there) */
static void pt_irq(void)
{
    if (!pt) return;
    if (!pt->irq) {
        pt->irq = 1;
        __asm__ volatile("dmb ish" ::: "memory");
        extern void aga_virtual_cia_irq(uint16_t bit, int active);
        aga_virtual_cia_irq(0x0008, 1);          /* INTF_PORTS: the Amiga's level 2 */
    }
}

/* ---- the board ------------------------------------------------------------ */
#define AC_NIB(n)   ((uint16_t)(((n) & 15u) << 12))
#define AC_BYTE(b)  AC_NIB((b) >> 4), AC_NIB(b)
#define AC_INV(b)   AC_NIB((~(b)) >> 4), AC_NIB(~(b))
static const uint16_t pt_rom[32] = {
    AC_BYTE(0xC1),                               /* 64 KB Zorro II, I/O, not memory */
    AC_INV(PT_PRODUCT),
    AC_INV(0x00), AC_INV(0x00),
    AC_INV(PT_MANUFACTURER >> 8), AC_INV(PT_MANUFACTURER & 0xff),
    AC_INV(0x55), AC_INV(0x53), AC_INV(0x42), AC_INV(0x50),   /* "USBP" */
    AC_INV(0x00), AC_INV(0x00), AC_INV(0x00), AC_INV(0x00),
    AC_INV(0x00), AC_INV(0x00)
};
extern void *tlsf;
extern void *tlsf_malloc_aligned(void *t, uintptr_t size, uintptr_t align);
extern void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high);
extern uintptr_t mmu_virt2phys(uintptr_t addr);
#define PT_MMU_ACCESS 0x400u
#define PT_MMU_ISHARE 0x300u
#define PT_MMU_EL0    0x040u
#define PT_MMU_NC     (2u << 2)

static void klog_flush_early(void);
static void *pt_mem;
static uintptr_t pt_phys;
static void pt_board_map(struct ExpansionBoard *board)
{
    if (!pt_mem) {
        pt_mem = tlsf_malloc_aligned(tlsf, PT_BOARD_BYTES, PT_BOARD_BYTES);
        if (!pt_mem) { kprintf("[FRF-XHCI] pass-through board: no memory\n"); return; }
        pt_phys = mmu_virt2phys((uintptr_t)pt_mem);
        mmu_map(pt_phys, (uintptr_t)pt_mem, PT_BOARD_BYTES, PT_MMU_ACCESS | PT_MMU_ISHARE | PT_MMU_NC, 0);
        __asm__ volatile("dsb ish; isb" ::: "memory");
        for (uint32_t i = 0; i < PT_BOARD_BYTES / 4; i++) ((volatile uint32_t *)pt_mem)[i] = 0;
    }
    /* each autoconfig (power-on, or a 68k RESET) maps the same memory where
       Kickstart put the board, and asks CPU1 for a fresh session */
    mmu_map(pt_phys, board->map_base, PT_BOARD_BYTES, PT_MMU_ACCESS | PT_MMU_ISHARE | PT_MMU_EL0 | PT_MMU_NC, 0);
    __asm__ volatile("dsb ish; isb" ::: "memory");
    volatile struct pt_hdr *h = pt_mem;
    h->magic = PT_MAGIC; h->version = PT_VERSION; h->nports = PT_PORTS; h->nreqs = PT_REQS;
    h->amiga_ver = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    if (!pt) {
        __atomic_store_n(&pt, h, __ATOMIC_RELEASE);
        klog_flush_early();                 /* the boot's lines first, then this one */
        kprintf("[FRF-XHCI] pass-through board at $%08x (alice68-usb.device finds it there)\n", board->map_base);
    }
    __atomic_store_n(&pt_session_req, 1, __ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
}
/* a USB mouse's wheels and extra buttons, for Alice68-Wheel (amiga/wheel.c), which turns them
   into AmigaOS's NewMouse events: the Amiga's mouse port has no wheel */
static void pt_mouse_extra(int wheel, int pan, unsigned buttons)
{
    volatile struct pt_hdr *h = __atomic_load_n(&pt, __ATOMIC_ACQUIRE);
    if (!h || (!wheel && !pan && h->mouse_btn == buttons)) return;
    h->wheel_y += wheel;
    h->wheel_x += pan;
    h->mouse_btn = buttons;
    __asm__ volatile("dmb ish" ::: "memory");
    h->mouse_seq++;
}
static struct ExpansionBoard pt_board = { pt_rom, PT_BOARD_BYTES, 0, 0, 1, pt_board_map };
static void * __attribute__((used, section(".boards.z2"))) pt_board_ref = &pt_board;

/* ---- the kernel log ring, for Alice68-USBLog in a Shell ------------------ */
static char klog_early[16384];          /* every kernel line until Kickstart configures the board */
static unsigned klog_early_n;
static volatile char klog_lock_v;          /* in cached kernel memory: exclusives are fine here */
static void klog_put(const char *s, unsigned n)
{
    while (__atomic_test_and_set(&klog_lock_v, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    if (pt) {
        volatile char *ring = (volatile char *)((uintptr_t)pt + PT_OFF_KLOG);
        uint32_t w = pt->klog_wr;
        for (unsigned i = 0; i < n; i++) ring[(w + i) % PT_KLOG_BYTES] = s[i];
        __asm__ volatile("dmb ish" ::: "memory");
        pt->klog_wr = w + n;
    } else {
        for (unsigned i = 0; i < n && klog_early_n < sizeof klog_early; i++) klog_early[klog_early_n++] = s[i];
    }
    __atomic_clear(&klog_lock_v, __ATOMIC_RELEASE);
}
/* every kernel log line (kprintf, support_rpi.c putByte, under print_lock) */
static char klog_line[240];
static unsigned klog_line_n;
void alice_klog_putc(char c)
{
    if (c == '\r') return;
    if (klog_line_n < sizeof klog_line - 1) klog_line[klog_line_n++] = c;
    if (c == '\n' || klog_line_n == sizeof klog_line - 1) {
        if (c != '\n') klog_line[klog_line_n++] = '\n';
        klog_put(klog_line, klog_line_n);
        klog_line_n = 0;
    }
}
static void klog_flush_early(void)
{
    unsigned n = klog_early_n;
    klog_early_n = 0;
    klog_put(klog_early, n);
}

/* ---- endpoints ------------------------------------------------------------ */
static int pt_ring_alloc(void)
{
    for (int i = 0; i < PT_NRING; i++)
        if (!(pt_ring_used[i / 32] & (1u << (i % 32)))) { pt_ring_used[i / 32] |= 1u << (i % 32); return i; }
    return -1;
}
static void pt_ring_free(int i) { if (i >= 0) pt_ring_used[i / 32] &= ~(1u << (i % 32)); }
static uintptr_t pt_ring_mem(int i) { return pool + P_PTRING + (uint32_t)i * 0x400u; }

static int ep_state(int slot, int dci)      /* from the output context: 1 running, 2 halted, 3 stopped, 4 error */
{
    return (int)(LE32(devs[slot].out[dci * (ctx_sz / 4)]) & 7u);
}

static void pt_done(int i, uint32_t actual, int err)
{
    volatile struct pt_req *r = pt_req(i);
    r->actual = actual;
    r->error = err;
    __asm__ volatile("dmb ish" ::: "memory");
    r->state = PTS_DONE;
    pt_irq();
}

/* Put an endpoint back to a fresh, empty ring: Stop it if it runs, Reset it
   if it halted, then Set TR Dequeue to the start. Whatever TRB it had is
   gone. Used by AbortIO, the NAK timeout and every error. */
static int control_buf_k(int slot, const uint8_t s[8], uintptr_t buf, int *actual);
static void pt_ep_flush(int slot, int dci, struct ring *r, uintptr_t mem)
{
    int st = ep_state(slot, dci);
    if (st == 1)
        (void)command(0, 0, (15u << 10) | ((uint32_t)dci << 16) | ((uint32_t)slot << 24), 0);   /* Stop Endpoint */
    st = ep_state(slot, dci);
    if (st == 2)
        (void)command(0, 0, (14u << 10) | ((uint32_t)dci << 16) | ((uint32_t)slot << 24), 0);   /* Reset Endpoint */
    ring_init(r, mem, NTRB_XFER);
    (void)command(r->phys | 1u, 0, (16u << 10) | ((uint32_t)dci << 16) | ((uint32_t)slot << 24), 0);   /* Set TR Dequeue */
}

/* the stored configuration: bConfigurationValue `val` (fetched when not the one held) */
static int pt_load_config(int slot, int val)
{
    struct pt_dev *p = &ptd[slot];
    volatile uint8_t *c = pt_cfg_of(slot);
    if (p->ncfg >= 9 && c[5] == val) return 0;
    for (int idx = 0; idx < (p->numcfg ? p->numcfg : 1); idx++) {
        uint8_t s[8] = { 0x80, 6, (uint8_t)idx, 2, 0, 0, 9, 0 };
        int got = 0;
        if (control_buf_k(slot, s, (uintptr_t)c, &got) != 1 || got < 9) continue;
        if (c[5] != val) continue;
        int total = c[2] | (c[3] << 8);
        if (total > 2048) total = 2048;
        s[6] = (uint8_t)total; s[7] = (uint8_t)(total >> 8);
        if (control_buf_k(slot, s, (uintptr_t)c, &got) == 1 && got >= 9) { p->ncfg = got; return 0; }
    }
    p->ncfg = 0;
    xlog("[FRF-XHCI] pass-through slot %d: configuration %d not found (device has %d)\n", slot, val, p->numcfg);
    return -1;
}

/* walk the stored configuration: the endpoints of (iface, alt), or of every
   interface's alt 0 when iface < 0 */
static int pt_walk(int slot, int iface, int alt, int dci_set[32], const uint8_t *eps[32], int ifs[32])
{
    struct pt_dev *p = &ptd[slot];
    volatile uint8_t *c = pt_cfg_of(slot);
    int n = 0, cur_if = -1, cur_alt = 0;
    for (int i = 0; i + 2 <= p->ncfg && c[i] >= 2; i += c[i]) {
        if (c[i + 1] == 4 && i + 9 <= p->ncfg) { cur_if = c[i + 2]; cur_alt = c[i + 3]; }
        else if (c[i + 1] == 5 && i + 7 <= p->ncfg) {
            int match = iface < 0 ? (cur_alt == 0) : (cur_if == iface && cur_alt == alt);
            if (!match) continue;
            uint8_t ea = c[i + 2];
            int dci = 2 * (ea & 15) + ((ea & 0x80) ? 1 : 0);
            if (dci < 2 || dci > 31) continue;
            dci_set[n] = dci; eps[n] = (const uint8_t *)&c[i]; ifs[n] = cur_if; n++;
            if (n >= 31) break;
        }
    }
    return n;
}

static void pt_fail_busy(int slot, int dci, int err)   /* the endpoint goes away under its request */
{
    struct pt_ep *pe = &ptd[slot].ep[dci];
    int i = pe->busy;
    if (i < 0) return;
    pe->busy = -1;
    pt_done(i, (uint32_t)ptk[i].off, err);
}

/* drop every endpoint of `drop_if` (all when -2), add those of (iface,alt)
   (iface -1: alt 0 of every interface; -3: nothing), one Configure Endpoint */
static int pt_configure(int slot, int drop_if, int add_if, int add_alt)
{
    struct dev *d = &devs[slot];
    struct pt_dev *p = &ptd[slot];
    volatile uint8_t *c = pt_cfg_of(slot);
    ctx_clear(d->in, 0x800);
    uint32_t addf = 1u, dropf = 0;
    for (int dci = 2; dci < 32; dci++)
        if (p->ep[dci].on && (drop_if == -2 || p->ep[dci].iface == drop_if)) {
            pt_fail_busy(slot, dci, IOERR_ABORTED);
            dropf |= 1u << dci;
            p->ep[dci].on = 0;
            pt_ring_free(p->ep[dci].ring); p->ep[dci].ring = -1;
        }
    int dcis[32], ifs[32]; const uint8_t *eps[32];
    int n = add_if == -3 ? 0 : pt_walk(slot, add_if, add_alt, dcis, eps, ifs);
    for (int k = 0; k < n; k++) {
        int dci = dcis[k];
        const uint8_t *e = eps[k];
        int at = e[3] & 3;
        if (at == 1) continue;                               /* isochronous: not offered */
        struct pt_ep *pe = &p->ep[dci];
        pe->ring = pt_ring_alloc();
        if (pe->ring < 0) { xlog("[FRF-XHCI] pass-through: out of transfer rings\n"); break; }
        ring_init(&pe->r, pt_ring_mem(pe->ring), NTRB_XFER);
        int mps = (e[4] | (e[5] << 8)) & 0x7ff, extra = (e[5] >> 3) & 3;
        int burst = 0, mult = 0;
        if (d->speed == 4 && e + 7 + 6 <= (const uint8_t *)c + p->ncfg && e[7] == 6 && e[8] == 0x30) burst = e[9];   /* SS companion */
        if (d->speed == 3 && at == 3) burst = extra;         /* high-bandwidth HS interrupt: Max Burst, Mult 0 */
        int in = (dci & 1) != 0;
        int type = at == 2 ? (in ? 6 : 2) : (in ? 7 : 3);
        int ival = at == 3 ? ep_interval(d->speed, e[6]) : 0;
        uint32_t esit = at == 3 ? (uint32_t)(mps * (burst + 1) * (mult + 1)) : 0;
        pe->c0 = ((uint32_t)ival << 16) | ((uint32_t)mult << 8);
        pe->c1 = (3u << 1) | ((uint32_t)type << 3) | ((uint32_t)burst << 8) | ((uint32_t)mps << 16);
        pe->c4 = ((esit & 0xffffu) << 16) | (at == 2 ? 3072u : (uint32_t)mps);
        volatile uint32_t *ep = ictx(d, dci + 1);
        ep[0] = LE32(pe->c0); ep[1] = LE32(pe->c1); ep[2] = LE32(pe->r.phys | 1u); ep[4] = LE32(pe->c4);
        pe->on = 1; pe->mps = mps; pe->type = at; pe->busy = -1;
        pe->iface = ifs[k];
        addf |= 1u << dci;
    }
    int maxdci = 1;
    for (int dci = 2; dci < 32; dci++) if (p->ep[dci].on) maxdci = dci;
    ictx(d, 0)[0] = LE32(dropf);
    ictx(d, 0)[1] = LE32(addf);
    ictx(d, 1)[0] = LE32(((uint32_t)maxdci << 27) | ((uint32_t)d->speed << 20) | (d->route & 0xfffffu));
    ictx(d, 1)[1] = LE32((uint32_t)d->port << 16);
    if (d->tt_slot) ictx(d, 1)[2] = LE32((uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8));
    int cc = command((uint32_t)(uintptr_t)d->in, 0, (12u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != 1) xlog("[FRF-XHCI] pass-through slot %d: Configure Endpoint failed (cc %d)\n", slot, cc);
    return cc == 1 ? 0 : -1;
}

/* ClearFeature(ENDPOINT_HALT): a halted endpoint is Reset (that clears the
   host toggle); a running one is dropped and added again, as Linux does,
   since only that resets its toggle to DATA0 like the device's */
static void pt_ep_clear_halt(int slot, int dci)
{
    struct dev *d = &devs[slot];
    struct pt_ep *pe = &ptd[slot].ep[dci];
    if (!pe->on) return;
    pt_fail_busy(slot, dci, IOERR_ABORTED);
    pe->flush = 0;
    if (ep_state(slot, dci) != 1) { pt_ep_flush(slot, dci, &pe->r, pt_ring_mem(pe->ring)); return; }
    ring_init(&pe->r, pt_ring_mem(pe->ring), NTRB_XFER);
    ctx_clear(d->in, 0x800);
    ictx(d, 0)[0] = LE32(1u << dci);
    ictx(d, 0)[1] = LE32((1u << dci) | 1u);
    int maxdci = 1;
    for (int k = 2; k < 32; k++) if (ptd[slot].ep[k].on) maxdci = k;
    ictx(d, 1)[0] = LE32(((uint32_t)maxdci << 27) | ((uint32_t)d->speed << 20) | (d->route & 0xfffffu));
    ictx(d, 1)[1] = LE32((uint32_t)d->port << 16);
    if (d->tt_slot) ictx(d, 1)[2] = LE32((uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8));
    volatile uint32_t *ep = ictx(d, dci + 1);
    ep[0] = LE32(pe->c0); ep[1] = LE32(pe->c1); ep[2] = LE32(pe->r.phys | 1u); ep[4] = LE32(pe->c4);
    (void)command((uint32_t)(uintptr_t)d->in, 0, (12u << 10) | ((uint32_t)slot << 24), 0);
}

static int pt_cc_err(int cc)
{
    switch (cc) {
    case 1: case 13: return 0;
    case 6:  return UHIOERR_STALL;
    case 3:  return UHIOERR_BABBLE;
    case 4:  return UHIOERR_XACTERROR;
    case -1: return UHIOERR_TIMEOUT;
    default: return UHIOERR_HOSTERROR;
    }
}

/* the 68k's buffer, as the kernel sees it (TTBR0 is the 68k's address space);
   every page checked - for writing when the transfer writes into it */
static int pt_buf_fast;                    /* the last pt_buf_ok(): all of it normal memory */
static int pt_mapped(uint32_t a, int wr)
{
    uint64_t par;
    if (wr) __asm__ volatile("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"((uintptr_t)a) : "memory");
    else    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"((uintptr_t)a) : "memory");
    if (par & 1) return 0;
    if (!((par >> 56) & 0xf0u)) pt_buf_fast = 0;            /* MAIR attribute 0x0X: device memory */
    return 1;
}
static int pt_buf_ok(uint32_t a, uint32_t len, int wr)
{
    pt_buf_fast = 1;
    if (!len) return 1;
    for (uint32_t p = a & ~0xfffu; p < a + len; p += 0x1000u) if (!pt_mapped(p, wr)) return 0;
    return 1;
}

/* ---- control ---------------------------------------------------------------- */
static int control_buf_k(int slot, const uint8_t s[8], uintptr_t buf, int *actual)
{
    struct dev *d = &devs[slot];
    uint16_t len = (uint16_t)(s[6] | (s[7] << 8));
    int in = (s[0] & 0x80) != 0;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_put(&d->ep0, (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24),
             (uint32_t)s[4] | ((uint32_t)s[5] << 8) | ((uint32_t)len << 16), 8, (2u << 10) | (1u << 6) | (trt << 16));
    uint32_t data_at = 0;
    if (len) data_at = ring_put(&d->ep0, (uint32_t)buf, 0, len, (3u << 10) | (in ? 1u << 16 : 0) | (in ? (1u << 2) : 0));   /* ISP: a short IN reports here */
    uint32_t status_at = ring_put(&d->ep0, 0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0 : 1u << 16));
    ring_db(slot, 1);
    int got = len, cc = -1;
    uint64_t end = now_ticks() + tick_hz() * 5u;
    for (;;) {
        if ((int64_t)(end - now_ticks()) <= 0) { cc = -1; break; }
        struct evt e = { 0, 0, 0, 0 };
        cc = wait_event(32, 0, slot, 1, &e, (uint32_t)((end - now_ticks()) * 1000u / tick_hz()) + 1u);
        if (cc < 0) break;
        if (data_at && e.p0 == data_at) {                   /* the data stage, short or failed */
            if (cc == 13) { got = len - (int)(e.st & 0xffffffu); continue; }
            if (cc != 1) break;
            continue;
        }
        if (e.p0 != status_at) continue;                    /* a stale event of an earlier transfer */
        if (cc == 1 || cc == 13) { *actual = got; return 1; }
        break;
    }
    /* any failure leaves EP0 halted or with TRBs on it: clean, for the next one */
    pt_ep_flush(slot, 1, &d->ep0, (uintptr_t)d->ep0.t);
    *actual = 0;
    return cc;
}

/* SuperSpeed devices shown as USB 2.0 ones: Neptune's classic interface has no SuperSpeed */
static void pt_fix_desc(int slot, volatile uint8_t *b, int n, uint16_t wvalue)
{
    if (devs[slot].speed != 4 || n < 2) return;
    if ((wvalue >> 8) == 1 && n >= 8) {                       /* device: USB 2.10, EP0 64 */
        b[2] = 0x10; b[3] = 0x02;
        b[7] = 64;
    } else if ((wvalue >> 8) == 2) {                          /* configuration: bulk at 512 */
        for (int i = 0; i + 2 <= n && b[i] >= 2; i += b[i])
            if (b[i + 1] == 5 && i + 6 <= n && (b[i + 3] & 3) == 2) { b[i + 4] = 0x00; b[i + 5] = 0x02; }
    }
}

static void pt_do_control(int i)
{
    volatile struct pt_req *r = pt_req(i);
    int slot = pt_port_slot[r->port];
    uint8_t s[8];
    for (int k = 0; k < 8; k++) s[k] = r->setup[k];
    uint16_t wv = (uint16_t)(s[2] | (s[3] << 8)), wi = (uint16_t)(s[4] | (s[5] << 8)), wl = (uint16_t)(s[6] | (s[7] << 8));
    int in = (s[0] & 0x80) != 0;
    if (wl > PT_MAXCHUNK || (wl && !pt_buf_ok(r->data, wl, in))) { pt_done(i, 0, UHIOERR_BADPARAMS); return; }

    /* standard requests that change what the xHC must know */
    if (s[0] == 0x00 && s[1] == 5) { pt_done(i, 0, 0); return; }                 /* SET_ADDRESS: the xHC did it */
    if (s[0] == 0x00 && s[1] == 9) {                                               /* SET_CONFIGURATION */
        if (wv && pt_load_config(slot, wv) < 0) { pt_done(i, 0, UHIOERR_STALL); return; }
        if (pt_configure(slot, -2, wv ? -1 : -3, 0) < 0 && wv) { pt_done(i, 0, UHIOERR_HOSTERROR); return; }
        ptd[slot].cfgval = wv;
        for (int k = 0; k < 32; k++) ptd[slot].alt[k] = 0;
    } else if (s[0] == 0x01 && s[1] == 11) {                                       /* SET_INTERFACE */
        if (pt_configure(slot, (int)wi, (int)wi, (int)wv) < 0) { pt_done(i, 0, UHIOERR_HOSTERROR); return; }
        if (wi < 32) ptd[slot].alt[wi] = (uint8_t)wv;
    } else if (s[0] == 0x02 && s[1] == 1 && wv == 0) {                             /* CLEAR_FEATURE(ENDPOINT_HALT) */
        int dci = 2 * (wi & 15) + ((wi & 0x80) ? 1 : 0);
        if (dci >= 2 && dci < 32) pt_ep_clear_halt(slot, dci);
    }

    uintptr_t bb = pt_bounce(i);
    int fast = wl ? (pt_buf_ok(r->data, wl, in), pt_buf_fast) : 1;
    if (wl && !in) pt_copy(bb, r->data, wl, fast);
    pt_dc(bb, wl);
    int got = 0;
    int cc = control_buf_k(slot, s, bb, &got);
    pt_dc(bb, wl);
    if (cc != 1) { pt_done(i, 0, pt_cc_err(cc)); return; }
    if (in && got > 0) {
        if (s[0] == 0x80 && s[1] == 6) pt_fix_desc(slot, (volatile uint8_t *)bb, got, wv);
        pt_copy(r->data, bb, (uint32_t)got, fast);
    }
    pt_done(i, (uint32_t)got, 0);
}

/* ---- bulk / interrupt -------------------------------------------------------- */
static void pt_arm_timeout(int i)
{
    volatile struct pt_req *r = pt_req(i);
    ptk[i].deadline = ((r->flags & UHFF_NAKTIMEOUT) && r->naktimeout)
                    ? now_ticks() + tick_hz() * (uint64_t)r->naktimeout / 1000u : 0;
}

static void pt_chunk(int i)
{
    volatile struct pt_req *r = pt_req(i);
    struct pt_k *k = &ptk[i];
    struct pt_ep *pe = &ptd[k->slot].ep[k->dci];
    uint32_t left = r->len - (uint32_t)k->off;
    uint32_t n = left > PT_MAXCHUNK ? PT_MAXCHUNK : left;
    uintptr_t bb = pt_bounce(i);
    int in = (k->dci & 1) != 0;
    if (!in) pt_copy(bb, r->data + (uint32_t)k->off, n, k->fast);
    pt_dc(bb, n);
    k->chunk = (int)n;
    k->trb = ring_put(&pe->r, (uint32_t)bb, 0, n, (1u << 10) | (1u << 5) | (in ? (1u << 2) : 0));
    pt_arm_timeout(i);
    ring_db(k->slot, k->dci);
}

static void pt_finish(int i, int err)
{
    struct pt_k *k = &ptk[i];
    ptd[k->slot].ep[k->dci].busy = -1;
    pt_done(i, (uint32_t)k->off, err);
}

static int pt_start(int i)       /* 1: started or finished, 0: endpoint busy - later */
{
    volatile struct pt_req *r = pt_req(i);
    int slot = pt_port_slot[r->port];
    int dci = 2 * (int)(r->ep & 15) + (r->dir == PTD_IN ? 1 : 0);
    struct pt_ep *pe = &ptd[slot].ep[dci];
    if (dci < 2 || dci > 31 || !pe->on) { r->state = PTS_BUSY; pt_done(i, 0, UHIOERR_BADPARAMS); return 1; }
    if (pe->busy >= 0) return 0;                             /* one at a time per endpoint, in order */
    if (!pt_buf_ok(r->data, r->len, r->dir == PTD_IN)) { r->state = PTS_BUSY; pt_done(i, 0, UHIOERR_BADPARAMS); return 1; }
    if (pe->flush) { pe->flush = 0; pt_ep_flush(slot, dci, &pe->r, pt_ring_mem(pe->ring)); }
    r->state = PTS_BUSY;
    pe->busy = i;
    ptk[i].slot = slot; ptk[i].dci = dci; ptk[i].off = 0; ptk[i].zlp = 0; ptk[i].fast = pt_buf_fast;
    pt_chunk(i);
    return 1;
}

static void pt_on_transfer(const struct evt *e)
{
    if (pt_session_req) return;                              /* the old session's: dropped */
    int slot = (int)(e->ctl >> 24), dci = (int)((e->ctl >> 16) & 31u), cc = (int)(e->st >> 24);
    if (dci < 2) return;
    struct pt_ep *pe = &ptd[slot].ep[dci];
    int i = pe->busy;
    if (i < 0) return;
    struct pt_k *k = &ptk[i];
    if (e->p0 != k->trb) return;                             /* not the TRB in flight: stale */
    volatile struct pt_req *r = pt_req(i);
    int in = (dci & 1) != 0;
    if (cc != 1 && cc != 13) {
        /* no commands here: this can run inside another command's wait.
           The endpoint is cleaned before its next transfer (pt_start), or
           by the ClearFeature(HALT) the class sends now. */
        pe->busy = -1;
        pe->flush = 1;
        pt_done(i, (uint32_t)k->off, pt_cc_err(cc));
        return;
    }
    if (k->zlp) { pt_finish(i, 0); return; }
    int got = k->chunk - (int)(e->st & 0xffffffu);
    if (got < 0) got = 0;
    if (in && got) { pt_dc(pt_bounce(i), (uint32_t)got); pt_copy(r->data + (uint32_t)k->off, pt_bounce(i), (uint32_t)got, k->fast); }
    k->off += got;
    if ((uint32_t)k->off < r->len && got == k->chunk) { pt_chunk(i); return; }
    if (!in && r->len && (r->len % (uint32_t)(pe->mps ? pe->mps : 512)) == 0 && !(r->flags & UHFF_NOSHORTPKT) && pe->type == 2) {
        k->zlp = 1;                                          /* terminating zero-length packet */
        k->trb = ring_put(&pe->r, (uint32_t)pt_bounce(i), 0, 0, (1u << 10) | (1u << 5));
        ring_db(slot, dci);
        return;
    }
    int err = 0;
    if (in && (uint32_t)k->off < r->len && !(r->flags & UHFF_ALLOWRUNTPKTS)) err = UHIOERR_RUNTPACKET;
    pt_finish(i, err);
}

/* AbortIO / NAK timeout: the request leaves the endpoint first, so the
   events the commands below dispatch can no longer touch it */
static void pt_abort(int i, int err)
{
    volatile struct pt_req *r = pt_req(i);
    if (r->state == PTS_SUBMIT) { r->state = PTS_BUSY; pt_done(i, 0, err); return; }
    struct pt_k *k = &ptk[i];
    struct pt_ep *pe = &ptd[k->slot].ep[k->dci];
    if (pe->busy != i) return;
    pe->busy = -1;
    pt_ep_flush(k->slot, k->dci, &pe->r, pt_ring_mem(pe->ring));
    pt_done(i, (uint32_t)k->off, err);
}

/* a 68k RESET: Kickstart configured the board again. Everything of the old
   OS goes: endpoints dropped (their TRBs with them), requests forgotten,
   ports offered afresh */
static void pt_new_session(void)
{
    for (int port = 1; port <= PT_PORTS; port++) {
        int slot = pt_port_slot[port];
        if (!slot) continue;
        for (int dci = 2; dci < 32; dci++) ptd[slot].ep[dci].busy = -1;
        (void)pt_configure(slot, -2, -3, 0);
    }
    __atomic_store_n(&pt_session_req, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < PT_REQS; i++) {
        volatile struct pt_req *r = pt_req(i);
        r->state = PTS_FREE; r->cancel = 0;
    }
    for (int port = 1; port <= PT_PORTS; port++) {
        int slot = pt_port_slot[port];
        volatile struct pt_port *pp = &pt->port[port - 1];
        if (slot) { pp->speed = (uint32_t)devs[slot].speed; pp->present = 1; }
        else      { pp->present = 0; }
        __asm__ volatile("dmb ish" ::: "memory");
        pp->gen++;
    }
    pt->port_gen++;
    pt->irq = 0;
    pt_irq();
    xlog("[FRF-XHCI] pass-through: new session for AmigaOS (%s)\n", "after autoconfig");
}

/* the main loop's turn: a new session, aborts, timeouts, new requests in
   submission order */
static void pt_service(void)
{
    if (!pt) return;
    if (pt_session_req) pt_new_session();
    int order[PT_REQS], n = 0;
    uint64_t now = now_ticks();
    for (int i = 0; i < PT_REQS; i++) {
        volatile struct pt_req *r = pt_req(i);
        uint32_t st = r->state;
        if (st != PTS_SUBMIT && st != PTS_BUSY) continue;
        __asm__ volatile("dmb ish" ::: "memory");
        if (r->cancel) { pt_abort(i, IOERR_ABORTED); continue; }
        if (st == PTS_BUSY) {
            if (ptk[i].deadline && (int64_t)(now - ptk[i].deadline) > 0) pt_abort(i, UHIOERR_NAKTIMEOUT);
            continue;
        }
        int k = n++;                                         /* insertion by seq */
        while (k > 0 && (int32_t)(pt_req(order[k - 1])->seq - r->seq) > 0) { order[k] = order[k - 1]; k--; }
        order[k] = i;
    }
    for (int j = 0; j < n; j++) {
        int i = order[j];
        volatile struct pt_req *r = pt_req(i);
        if (r->state != PTS_SUBMIT) continue;
        if (r->port < 1 || r->port > PT_PORTS || !pt_port_slot[r->port]) { r->state = PTS_BUSY; pt_done(i, 0, UHIOERR_TIMEOUT); continue; }
        if (r->type == PTT_CONTROL) { r->state = PTS_BUSY; pt_do_control(i); }
        else (void)pt_start(i);
    }
}

/* ---- devices in and out ------------------------------------------------------ */
static void pt_attach(int slot, uint16_t vid, uint16_t pid, const char *where)
{
    int port = 0;
    for (int p = 1; p <= PT_PORTS; p++) if (!pt_port_slot[p]) { port = p; break; }
    if (!port) { xlog("[FRF-XHCI] %s: no free pass-through port (max %d)\n", where, PT_PORTS); return; }
    struct dev *d = &devs[slot];
    struct pt_dev *p = &ptd[slot];
    for (unsigned b = 0; b < sizeof *p; b++) ((uint8_t *)p)[b] = 0;
    for (int k = 0; k < 32; k++) { p->ep[k].ring = -1; p->ep[k].busy = -1; }
    p->numcfg = d->numcfg ? d->numcfg : 1;                   /* from enumerate()'s device descriptor */
    p->port = port;
    d->pt = 1;
    pt_port_slot[port] = slot;
    if (pt) {
        volatile struct pt_port *pp = &pt->port[port - 1];
        pp->speed = (uint32_t)d->speed;
        pp->vidpid = ((uint32_t)vid << 16) | pid;
        pp->present = 1;
        __asm__ volatile("dmb ish" ::: "memory");
        pp->gen++;                                           /* after present: the Amiga reads gen first */
        pt->port_gen++;
        pt_irq();
    }
    xlog("[FRF-XHCI] %s: %04x:%04x -> Neptune, alice68-usb.device port %d\n", where, vid, pid, port);
}

static void pt_detach(int slot)
{
    struct pt_dev *p = &ptd[slot];
    if (!devs[slot].pt || !p->port) return;
    int port = p->port;
    for (int dci = 2; dci < 32; dci++) pt_fail_busy(slot, dci, UHIOERR_TIMEOUT);
    if (pt) {
        pt->port[port - 1].present = 0;
        __asm__ volatile("dmb ish" ::: "memory");
        pt->port[port - 1].gen++;
        pt->port_gen++;
        for (int i = 0; i < PT_REQS; i++) {
            volatile struct pt_req *r = pt_req(i);
            if (r->state == PTS_SUBMIT && r->port == (uint32_t)port) { r->state = PTS_BUSY; pt_done(i, 0, UHIOERR_TIMEOUT); }
        }
        pt_irq();
    }
    for (int k = 0; k < 32; k++) { pt_ring_free(p->ep[k].ring); p->ep[k].ring = -1; p->ep[k].on = 0; p->ep[k].busy = -1; }
    pt_port_slot[port] = 0;
    p->port = 0;
    devs[slot].pt = 0;
    xlog("[FRF-XHCI] pass-through port %d: device gone\n", port);
}
