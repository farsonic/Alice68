/* AGA-PISTORM - a long hard look at the Pi's own USB controller, the way the
 * user's driver set the bastard up.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The Pi's USB mice and pads belong to Poseidon and the driver the user
 * installed - on a Pi 4 or a CM4 rondoval's xhci.device. In a WHDLoad game
 * the OS stops, and that driver with it. The plan (aga_pad.c's words for the
 * game, from the Pi's USB instead of the A314) is to borrow the device that
 * driver already set up, for the game's length only: a secondary interrupter
 * of mine, and a detour from the driver's next free slot on the mouse's
 * transfer ring into a ring of mine, joined back at the leave. No xHCI
 * command at all - those go through the driver's command and event rings, and
 * I'm not touching them.
 *
 * Step 1 only LOOKS. Selector $1806 (aga_diag.c) runs it and reads back the
 * text. Nothing is written to the controller; the driver's memory is only
 * read. What it reports is what the plan rests on: the controller and its
 * interrupters (the driver uses the first only), the device slots, and for
 * each interrupt IN endpoint its ring - where the controller stands and where
 * the driver would write next.
 *
 * What it found on the CM4 (30.10.2026): ONE interrupter, the driver's. So
 * step 2, the mouse and the pad in a game (below), raises no event at all:
 * its transfers have IOC and ISP clear, and a report is told by its buffer
 * no longer holding the pattern it was filled with. Subtle as a brick, and
 * just as reliable.
 *
 * The onboard xHCI of the BCM2711 is /scb/xhci: the CM4's USB port, and the
 * Pi 4's USB-C one, with otg_mode=1 in config.txt. The Pi 4's USB-A ports are
 * the VL805 behind PCIe, not looked at here. Its DMA addresses are the 68k's
 * own: rondoval's driver writes its pointers as they are (xhci.c,
 * xhci_writeq(&hcor->or_dcbaap, (dma_addr_t)ctrl->dcbaa)), and fast RAM is
 * mapped 1:1, so they are read as 68k addresses here - each page checked
 * with AT first, so a wrong pointer reports instead of faulting. I'd rather
 * read a complaint than stare at a dead machine.
 */
#include <stdint.h>
#include <stdarg.h>
#include "devicetree.h"

typedef void (*putc_func)(void *data, char c);
extern void vkprintf_pc(putc_func putc_f, void *putc_data, const char *format, va_list args);

#define LE32(x) __builtin_bswap32(x)                   /* the ARM runs big-endian, because nothing is ever easy */

/* ---- the report, and not a byte more -------------------------------------- */

#define OUT_SIZE 6144
static char out[OUT_SIZE];
static uint32_t out_len;

static void out_c(void *d, char c) { (void)d; if (out_len < OUT_SIZE - 1) out[out_len++] = c; }

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vkprintf_pc(out_c, 0, fmt, ap);
    va_end(ap);
    out_c(0, '\n');
}

/* ---- reading, never writing - I'm a guest here, not an arsonist ----------- */

static inline uint32_t mmio(uintptr_t a) { return LE32(*(volatile uint32_t *)a); }

/* the page is mapped for reading (AT S1E1R: PAR_EL1.F clear, the usual drill) */
static int mapped(uintptr_t a)
{
    uint64_t par;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(a) : "memory");
    return !(par & 1);
}

/* a little-endian long of the driver's memory, or 0 with *ok cleared under it */
static uint32_t mem32(uint64_t a, int *ok)
{
    if ((a & 3) || a >= (1ull << 40) || !mapped((uintptr_t)a)) { *ok = 0; return 0; }
    return LE32(*(volatile uint32_t *)(uintptr_t)a);
}

/* What the controller writes (the output contexts) is read past the cache:
   the driver never writes those lines, so none is dirty, and clean +
   invalidate only drops a stale copy. Don't touch what you don't own. */
static void fresh(uint64_t a, uint32_t len)
{
    for (uint64_t l = a & ~63ull; l < a + len; l += 64)
        if (mapped((uintptr_t)l)) __asm__ volatile("dc civac, %0" :: "r"((uintptr_t)l) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* /scb's child address to my virtual one: Emu68 maps the ranges low and
   rewrites their CPU addresses (raspi/start_rpi64.c map_peripheral_ranges) */
static uintptr_t scb_virt(uint32_t bus)
{
    of_node_t *e = dt_find_node("/scb");
    of_property_t *p = e ? dt_find_property(e, "ranges") : 0;
    if (!p) return 0;
    int ac = (int)dt_get_property_value_u32(e->on_parent, "#address-cells", 1, 0);
    int ab = (int)dt_get_property_value_u32(e, "#address-cells", 1, 1);
    int sb = (int)dt_get_property_value_u32(e, "#size-cells", 1, 1);
    int n = ab + ac + sb;
    uint32_t *r = p->op_value;
    for (int32_t len = (int32_t)p->op_length; len >= n * 4; len -= n * 4, r += n) {
        uint32_t b = r[ab - 1], c = r[ab + ac - 1], l = r[n - 1];   /* big-endian, as the ARM insists */
        if (b == c) continue;                      /* identity ranges are not mapped, so skip the useless buggers */
        if (bus - b < l) return (uintptr_t)c + (bus - b);
    }
    return 0;
}

static const char *const ep_type[8] = { "-", "isoc out", "bulk out", "int out", "control", "isoc in", "bulk in", "int in" };
static const char *const ep_state[8] = { "disabled", "running", "halted", "stopped", "error", "5", "6", "7" };
static const char *const speed_name[8] = { "?", "full", "low", "high", "super", "super+", "6", "7" };

/* One interrupt IN endpoint's ring, laid bare: the segment the controller's
   pointer is in, its TRBs' cycle bits as runs, the link at its end. The
   driver's next slot is where the cycle bit changes after the TRBs it gave
   the controller, and yes, that took me a while to see. */
static void show_ring(uint64_t deq, int dcs)
{
    uint64_t seg = deq & ~0xFFFull;                /* 256 TRBs, 4 KB aligned (xhci.h says so, I checked) */
    int ok = 1, run_c = -1, run_from = 0, runs = 0;
    say("      ring segment %08x%08x, the controller's pointer at TRB %d (DCS %d)",
        (uint32_t)(seg >> 32), (uint32_t)seg, (int)((deq - seg) >> 4), dcs);
    for (int i = 0; i < 256 && ok; i++) {
        int c = (int)(mem32(seg + 16u * i + 12, &ok) & 1);
        if (c == run_c) continue;
        if (run_c >= 0 && runs++ < 12) say("        TRBs %3d-%3d cycle %d", run_from, i - 1, run_c);
        run_c = c;
        run_from = i;
    }
    if (!ok) { say("        (not readable)"); return; }
    say("        TRBs %3d-255 cycle %d%s", run_from, run_c, runs >= 12 ? " (more runs not shown)" : "");
    /* the TRB at the pointer, the two after it, and the link - the whole sad story */
    for (int k = 0; k < 3; k++) {
        uint64_t t = deq + 16u * k;
        if (t >= seg + 255 * 16) break;
        uint32_t p0 = mem32(t, &ok), p1 = mem32(t + 4, &ok), st = mem32(t + 8, &ok), ctl = mem32(t + 12, &ok);
        say("        @%3d type %2u cyc %u ioc %u len %u buf %08x%08x", (int)((t - seg) >> 4),
            (ctl >> 10) & 63, ctl & 1, (ctl >> 5) & 1, st & 0x1FFFF, p1, p0);
    }
    uint64_t lk = seg + 255 * 16;
    uint32_t l0 = mem32(lk, &ok), l1 = mem32(lk + 4, &ok), lctl = mem32(lk + 12, &ok);
    if (ok) say("        @255 type %2u cyc %u tc %u to %08x%08x%s", (lctl >> 10) & 63, lctl & 1, (lctl >> 1) & 1,
                l1, l0 & ~15u, (((uint64_t)l1 << 32) | (l0 & ~15u)) == seg ? " (itself: one segment)" : " (more segments)");
}

static void um_say(void);                          /* step 2's state; it's down below, patience */

static void probe(void)
{
    out_len = 0;
    say("AGA-PISTORM USB look (read only)");
    of_node_t *x = dt_find_node("/scb/xhci");
    if (!x) { say("no /scb/xhci in the device tree: not a Pi 4 / CM4 (a Pi 3 has the DWC2 only)"); return; }
    of_property_t *st = dt_find_property(x, "status");
    const char *status = st && st->op_value ? (const char *)st->op_value : "okay";
    say("/scb/xhci status: %s", status);
    /* disabled: no otg_mode=1, the block may be unclocked - a read could hang the
   bus, and I'm not debugging that today */
    if (!(status[0] == 'o' && status[1] == 'k')) { say("not enabled (otg_mode=1 in config.txt?): not touched"); return; }
    of_property_t *rg = dt_find_property(x, "reg");
    int ab = (int)dt_get_property_value_u32(dt_find_node("/scb"), "#address-cells", 1, 1);
    uint32_t bus = 0;
    if (rg && ab >= 1 && rg->op_length >= 4u * (uint32_t)ab) bus = ((uint32_t *)rg->op_value)[ab - 1];   /* its low cell, the only one I need */
    uintptr_t base = bus ? scb_virt(bus) : 0;
    say("reg %08x -> ours %p", bus, (void *)base);
    if (!base || !mapped(base)) { say("not mapped: nothing more"); return; }

    uint32_t cap = mmio(base), hcs1 = mmio(base + 4), hcs2 = mmio(base + 8), hcc1 = mmio(base + 0x10);
    uint32_t dboff = mmio(base + 0x14) & ~3u, rtsoff = mmio(base + 0x18) & ~31u;
    uint32_t caplen = cap & 0xFF, max_slots = hcs1 & 0xFF, max_intrs = (hcs1 >> 8) & 0x7FF, max_ports = hcs1 >> 24;
    int ctx = (hcc1 & 4) ? 64 : 32;
    say("xHCI %x.%02x  slots %u  interrupters %u  ports %u  ERST max %u  AC64 %u  context %d  dboff %x rtsoff %x",
        cap >> 24, (cap >> 16) & 0xFF, max_slots, max_intrs, max_ports, 1u << ((hcs2 >> 4) & 15), hcc1 & 1, ctx,
        dboff, rtsoff);
    if (caplen < 0x20 || caplen > 0x80) { say("odd CAPLENGTH %u: nothing more", caplen); return; }

    uintptr_t op = base + caplen, rt = base + rtsoff;
    uint32_t cmd = mmio(op), sts = mmio(op + 4), cfg = mmio(op + 0x38);
    uint64_t dcbaa = ((uint64_t)mmio(op + 0x34) << 32) | mmio(op + 0x30);
    say("USBCMD %08x (run %u, irq %u)  USBSTS %08x (halted %u)  slots on %u  DCBAA %08x%08x",
        cmd, cmd & 1, (cmd >> 2) & 1, sts, sts & 1, cfg & 0xFF, (uint32_t)(dcbaa >> 32), (uint32_t)dcbaa);
    for (uint32_t p = 1; p <= max_ports && p <= 8; p++) {
        uint32_t sc = mmio(op + 0x400 + 0x10 * (p - 1));
        say("port %u: %08x  connected %u enabled %u speed %s", p, sc, sc & 1, (sc >> 1) & 1, speed_name[(sc >> 10) & 7]);
    }
    for (uint32_t i = 0; i < max_intrs && i < 4; i++) {
        uintptr_t ir = rt + 0x20 + 32 * i;
        uint32_t iman = mmio(ir), erstsz = mmio(ir + 8) & 0xFFFF;
        uint64_t erstba = ((uint64_t)mmio(ir + 0x14) << 32) | (mmio(ir + 0x10) & ~63u);
        uint64_t erdp = ((uint64_t)mmio(ir + 0x1C) << 32) | mmio(ir + 0x18);
        say("interrupter %u: IMAN %x  ERSTSZ %u  ERSTBA %08x%08x  ERDP %08x%08x%s", i, iman, erstsz,
            (uint32_t)(erstba >> 32), (uint32_t)erstba, (uint32_t)(erdp >> 32), (uint32_t)erdp,
            (i && !erstsz && !erstba) ? "  (unused)" : "");
    }
    if (!(cmd & 1) || (sts & 1)) { say("the controller is not running (no driver on it): nothing more"); return; }

    int ok = 1;
    uint32_t on = cfg & 0xFF;
    for (uint32_t s = 1; s <= on && s <= 32; s++) {
        uint64_t dc = mem32(dcbaa + 8 * s, &ok);
        dc |= (uint64_t)mem32(dcbaa + 8 * s + 4, &ok) << 32;
        if (!ok) { say("DCBAA not readable at %08x%08x: nothing more", (uint32_t)(dcbaa >> 32), (uint32_t)dcbaa); return; }
        dc &= ~63ull;
        if (!dc) continue;
        fresh(dc, 32u * (uint32_t)ctx);
        uint32_t s0 = mem32(dc, &ok), s1 = mem32(dc + 4, &ok), s2 = mem32(dc + 8, &ok), s3 = mem32(dc + 12, &ok);
        if (!ok) { say("slot %u: context %08x%08x not readable", s, (uint32_t)(dc >> 32), (uint32_t)dc); continue; }
        uint32_t entries = s0 >> 27;
        say("slot %u: address %u  state %u  speed %s  root port %u  route %05x  hub %u  TT slot %u port %u  entries %u",
            s, s3 & 0xFF, s3 >> 27, speed_name[(s0 >> 20) & 7], (s1 >> 16) & 0xFF, s0 & 0xFFFFF, (s0 >> 26) & 1,
            s2 & 0xFF, (s2 >> 8) & 0xFF, entries);
        for (uint32_t d = 1; d <= entries && d < 32; d++) {
            uint64_t e = dc + (uint64_t)ctx * d;
            uint32_t e0 = mem32(e, &ok), e1 = mem32(e + 4, &ok), e2 = mem32(e + 8, &ok), e3 = mem32(e + 12, &ok);
            uint32_t t = (e1 >> 3) & 7;
            if (!ok || !t) continue;
            uint64_t deq = ((uint64_t)e3 << 32) | (e2 & ~15u);
            say("    ep %u%s (DCI %u): %s  %s  max packet %u  interval %u  TR %08x%08x DCS %u",
                d / 2, d == 1 ? "" : (d & 1) ? " in" : " out", d, ep_type[t], ep_state[e0 & 7], e1 >> 16,
                (e0 >> 16) & 0xFF, e3, e2 & ~15u, e2 & 1);
            if (t == 7) show_ring(deq, e2 & 1);
        }
    }
    um_say();
    say("end");
}

/* ---- step 2: the mouse and the pad in a game, where it gets hairy -----------
 *
 * agaboot names them at each game's entry, while the OS still runs: a block
 * in fast RAM ($DFF1EC/$DFF1EE, then $DFF1F2 = $5BA1 for the mouse, $5BA3 for
 * the pad; $5BA0 / $5BA2 forget it), copied here at once. The mouse's -
 *
 *   +0  'USBM'
 *   +4  USB address, DCI of its interrupt IN endpoint, report ID (0 none),
 *       divisor (counts per Amiga count)
 *   +8  buttons: first bit (word), how many (byte), flags (byte: 1 log the
 *       first reports, 2 found where +20 says)
 *   +12 X: first bit (word), bits (byte), 0
 *   +16 Y: first bit (word), bits (byte), 0
 *   +20 with flags 2: the root port (byte), the route string below it (3
 *       bytes, 20 bits) - the USB address then 0
 *
 * and the pad's -
 *
 *   +0  'USBP'
 *   +4  USB address, DCI, report ID (0 none), flags (1 log every report
 *       that changes, 2 found where +36 says, 4 the ranges at +40)
 *   +8  hat: first bit (word), bits (byte, 0 none), up's value (byte, flags
 *       4; else 0) - up, clockwise to up + 7, anything else centred
 *   +12 stick X: first bit (word), bits (byte, 0 none), 0 - the outer
 *       quarters of its range are the directions
 *   +16 stick Y: the same
 *   +20 red, blue, green, yellow, forward, reverse, play: each one's bit
 *       (word, $FFFF none)
 *   +36 with flags 2: the root port, the route string, as the mouse's +20
 *   +40 with flags 4: X's logical minimum and maximum, then Y's (longs;
 *       signed below 0; minimum not below maximum: all its bits, unsigned)
 *
 * agaboot finds them through Poseidon (usbfind.c), whose USB addresses are
 * its own, not the controller's, so it says where they are plugged in. By
 * hand (agaboot USBMOUSE / USBPAD) they may still come by address.
 *
 * bits counted from the report's first byte, the ID included, as HID packs
 * them (little-endian; the mouse's X and Y signed). The mouse plays in port
 * 0, the pad in port 1 (aga_pad.c, with the A314's words there).
 *
 * When WHDLoad takes the machine (aga_usb_taken, core 0, from the INTENA
 * clear that sets game_taken) the driver has a transfer queued on each
 * device's ring, the newest at TRB k, and its next free slot is k + 1, the
 * cycle bit there still the last lap's. A Link TRB there, given to the
 * controller, sends it into a ring of mine once the reports the driver's
 * TRBs wait for have come (into the driver's buffers, with the driver's
 * events: its own, handled after the game). My ring holds one transfer at a
 * time, as the driver's does: when its buffer changes, the housekeeper core
 * re-fills it with the pattern, gives the controller the next TRB and rings
 * the slot's doorbell - a doorbell is no command. The TRB after the one given
 * is never the controller's, so it can always be rewritten.
 *
 * At the leave (aga_usb_leave, core 0, before Emu68's held interrupts reach
 * the driver) that next TRB becomes a Link back to k + 1, toggling the cycle
 * if my laps left it the other way; and k + 1, if it still holds my Link,
 * gets back what it held, so a controller that never came in finds it empty
 * and waits for the driver. The report my last TRB takes is lost. The
 * controller may still sit in my ring then, waiting for that report, so a
 * ring is used again only once it is surely out (um_free).
 *
 * Errors would still post an event to the driver's ring, naming a TRB of
 * mine - a pulled receiver, say. That is the risk left. A device the driver
 * dropped meanwhile (its slot's context gone from the DCBAA, or its endpoint
 * disabled) is left alone at the leave: its ring may be free memory by then.
 *
 * The driver must not queue on that ring while my Link is in: rondoval's
 * driver, as Linux's, tells a Link TRB by its type at its own next free slot
 * (last_trb()), and k + 1 is that slot - its next transfer would go to TRB 0,
 * where the controller never comes, with my Link's cycle turned. In a game
 * it does not run until the Link is gone again.
 *
 * So agaboot's Controller window does not borrow the pad: the driver runs
 * then, and a borrow left Poseidon's pad dead (the probe of 31.10.2026 found
 * just that). While the window is open on the PiStorm USB ($5BA5, ended at
 * $5BA4) the housekeeper only reads the pad's ring (peek). Poseidon's
 * hid.class keeps one transfer queued; its buffer is the driver's own, taken
 * for that transfer and freed as soon as the report is copied out - the
 * free list's pointers land on the report, so a transfer seen done is read
 * too late (31.10.2026: 46 reads, not one with the report's ID). The
 * housekeeper reads the queued transfer's buffer at each of its visits,
 * microseconds apart, against what it held when the transfer was queued:
 * the controller's write changes it well before the 68k has taken the
 * interrupt, and that once is the report, read as in a game for the
 * window's tester. Nothing is written. A game started meanwhile ends the
 * look and borrows the pad. Ugly as sin, but it's what the hardware gives me. */

#define UM_MAGIC   0x5553424Du                     /* 'USBM' - the mouse's handshake, don't ask */
#define UP_MAGIC   0x55534250u                     /* 'USBP' - the pad's */
#define UM_DEVS    2                               /* the mouse, then the pad - two, don't get creative */
#define UM_RINGS   6
#define UM_TRBS    16                              /* 15 transfers, then the link back to the first */
#define UM_BUF     64
#define UM_SENT    0xA5u                           /* a buffer the controller has not written, marked like a plague house */
#define TYPE_GAMECTLR 1u                           /* aga_pad.c's words, same damn numbers */
#define TYPE_MOUSE 2u
#define JPF_RIGHT  0x1u
#define JPF_LEFT   0x2u
#define JPF_DOWN   0x4u
#define JPF_UP     0x8u
#define JPF_RED    (1u << 22)
#define JPF_BLUE   (1u << 23)
#define JPF_PLAY   (1u << 17)

/* the pad's buttons as the block lists them: red, blue, green, yellow,
   forward, reverse, play. That order, don't reshuffle the poor sods. */
static const uint32_t pad_jpf[7] = { 1u << 22, 1u << 23, 1u << 20, 1u << 21, 1u << 19, 1u << 18, 1u << 17 };
static const uint8_t hat_dir[8] = { JPF_UP, JPF_UP | JPF_RIGHT, JPF_RIGHT, JPF_DOWN | JPF_RIGHT,
                                    JPF_DOWN, JPF_DOWN | JPF_LEFT, JPF_LEFT, JPF_UP | JPF_LEFT };
static const char *const dev_name[UM_DEVS] = { "mouse", "pad" };

extern void *tlsf;
extern void *tlsf_malloc(void *handle, uintptr_t size);
extern uintptr_t mmu_virt2phys(uintptr_t virt);
extern void aga_diag_log(const char *line);
extern int aga_glue_game_taken(void);
extern void aga_pad_usb_mouse(uint32_t w);
extern void aga_pad_usb_pad(uint32_t w);

struct um_ring {
    uint8_t *mem;                                  /* the TRBs first, then the buffers - one allocation, one pointer */
    uint64_t dma;
    int busy;                                      /* the controller may still be poking around in it */
    uint32_t slot, dci;                            /* the endpoint it hung off, remember that */
    uint64_t seg;                                  /* ...the driver's ring on that endpoint, */
    int k;                                         /* ...and which of the driver's TRBs that was */
    int cons;                                      /* the TRB handed over at the leave */
};

struct um_dev {
    int have;                                      /* named by agaboot, for better or worse */
    uint8_t addr, dci, id, flags;
    uint8_t div, b_n, x_bits, y_bits;              /* the mouse's half */
    uint16_t b_off, x_off, y_off;
    uint8_t hat_bits, sx_bits, sy_bits, hat_min;   /* the pad's half */
    uint16_t hat_off, sx_off, sy_off, btn[7];
    int32_t sx_min, sx_max, sy_min, sy_max;        /* stick ranges: min >= max means all bits, unsigned - don't overthink it */
    uint8_t root;                                  /* found by where it's plugged in: the root port, */
    uint32_t route;                                /* ...and the route string below it; root 0: by address */
    struct um_ring *cur;                           /* the detour, while it's in */
    int cons, ccs, c0, drv_k, maxp;
    uint32_t slot;
    uint64_t drv_seg;
    uint64_t dcbaa_e, dc;                          /* its DCBAA entry, then its device context */
    int ctx;
    uint32_t entry_trb[4], saved_trb[4];
    uintptr_t db_reg;
    unsigned reports, logged, missed;
    uint64_t pk_a;                                 /* peek mode: the queued transfer's buffer, */
    uint32_t pk_len;                               /* ...its length (0: nothing watched), */
    uint8_t pk_was[UM_BUF];                        /* ...what it held when it was queued */
    uint32_t last_b;
    int32_t acc_x, acc_y;
    uint8_t cnt_x, cnt_y, last[16];
    uint64_t next_look;
};
static struct um_dev devs[UM_DEVS];
static struct um_ring rings[UM_RINGS];
static volatile int um_lock;
volatile int aga_usb_live;                         /* bit n: device n's detour is in (aga_pad.c gives it to the game) */
static volatile int um_tester;                     /* bit n: device n read for agaboot's Controller window, peek only */

static inline int  live(int n)   { return (__atomic_load_n(&aga_usb_live, __ATOMIC_ACQUIRE) >> n) & 1; }
static inline void um_take(void) { while (__atomic_exchange_n(&um_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static inline int  um_try(void)  { return !__atomic_exchange_n(&um_lock, 1, __ATOMIC_ACQUIRE); }
static inline void um_give(void) { __atomic_store_n(&um_lock, 0, __ATOMIC_RELEASE); }

/* a diag log line, formatted on the caller's stack: core 0 and the
   housekeeper both log, and I'm not having a heap fight in that path */
struct um_line { char s[160]; uint32_t n; };
static void um_c(void *d, char c) { struct um_line *l = d; if (l->n < sizeof l->s - 1) l->s[l->n++] = c; }
static void um_log(const char *fmt, ...)
{
    struct um_line l;
    va_list ap;
    l.n = 0;
    va_start(ap, fmt);
    vkprintf_pc(um_c, &l, fmt, ap);
    va_end(ap);
    l.s[l.n] = 0;
    aga_diag_log(l.s);
}

static const char *um_fmt(struct um_line *l, const char *fmt, ...)
{
    va_list ap;
    l->n = 0;
    va_start(ap, fmt);
    vkprintf_pc(um_c, l, fmt, ap);
    va_end(ap);
    l->s[l->n] = 0;
    return l->s;
}

/* how a device is found, for the log: "address 2" or "port 1 route 00002" */
static const char *um_at(const struct um_dev *u, struct um_line *l)
{
    return u->root ? um_fmt(l, "port %u route %05x", u->root, u->route) : um_fmt(l, "address %u", u->addr);
}

/* a line out to RAM, past every cache, before the controller gets its hands on it */
static inline void um_clean(const volatile void *p, uint32_t len)
{
    for (uintptr_t l = (uintptr_t)p & ~63ul; l < (uintptr_t)p + len; l += 64)
        __asm__ volatile("dc civac, %0" :: "r"(l) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline volatile uint32_t *trb(struct um_ring *r, int i) { return (volatile uint32_t *)(r->mem + 16 * i); }
static inline volatile uint8_t *buf(struct um_ring *r, int i) { return r->mem + 16 * UM_TRBS + UM_BUF * i; }

/* a TRB written in two steps: its pointer and status first, its control
   (with the cycle bit that hands it over) last. Order matters, as always. */
static void put_trb(volatile uint32_t *t, uint64_t ptr, uint32_t status, uint32_t ctl)
{
    t[0] = LE32((uint32_t)ptr);
    t[1] = LE32((uint32_t)(ptr >> 32));
    t[2] = LE32(status);
    um_clean(t, 16);
    t[3] = LE32(ctl);
    um_clean(t, 16);
}
#define CTL_NORMAL(c)        ((1u << 10) | (uint32_t)(c))            /* IOC, ISP clear: no event, exactly as step 1 taught me */
#define CTL_LINK(c, tc)      ((6u << 10) | ((tc) ? 2u : 0u) | (uint32_t)(c))

/* The xHCI's DMA has to reach it: an identity range of /scb's dma-ranges */
static int dma_ok(uint64_t a, uint64_t len)
{
    of_node_t *e = dt_find_node("/scb");
    of_property_t *p = e ? dt_find_property(e, "dma-ranges") : 0;
    if (!p) return 0;
    int ac = (int)dt_get_property_value_u32(e->on_parent, "#address-cells", 1, 0);
    int ab = (int)dt_get_property_value_u32(e, "#address-cells", 1, 1);
    int sb = (int)dt_get_property_value_u32(e, "#size-cells", 1, 1);
    int n = ab + ac + sb;
    uint32_t *r = p->op_value;
    for (int32_t left = (int32_t)p->op_length; left >= n * 4; left -= n * 4, r += n) {
        uint64_t b = 0, c = 0, l = 0;
        for (int i = 0; i < ab; i++) b = b << 32 | r[i];
        for (int i = 0; i < ac; i++) c = c << 32 | r[ab + i];
        for (int i = 0; i < sb; i++) l = l << 32 | r[ab + ac + i];
        if (b == c && a >= c && a + len <= c + l) return 1;
    }
    return 0;
}

static int um_alloc(void)
{
    for (int i = 0; i < UM_RINGS; i++) {
        if (rings[i].mem) continue;
        uint32_t need = 16 * UM_TRBS + UM_BUF * (UM_TRBS - 1);
        uint8_t *m = tlsf_malloc(tlsf, need + 512);
        if (!m) return 0;
        m = (uint8_t *)(((uintptr_t)m + 255) & ~(uintptr_t)255);   /* within one 64 KB: 256-aligned, 1.2 KB of it */
        if (((uintptr_t)m & 0xFFFF) + need > 0x10000) m += 256;
        uint64_t d = mmu_virt2phys((uintptr_t)m);
        if (!dma_ok(d, need)) { um_log("[AGA] usb: our memory at %08x%08x is not the xHCI's to reach",
                                       (uint32_t)(d >> 32), (uint32_t)d); return 0; }
        rings[i].mem = m;
        rings[i].dma = d;
    }
    return 1;
}

/* the driver's newest TRB on a one-segment ring: the last before the cycle
   bit changes; -1 if it never does (TRB 254, the link next: not used) */
static int newest(uint64_t seg, int *c)
{
    int ok = 1;
    fresh(seg, 4096);
    int c0_ = (int)(mem32(seg + 12, &ok) & 1);
    for (int i = 1; i < 255 && ok; i++)
        if ((int)(mem32(seg + 16u * i + 12, &ok) & 1) != c0_) { *c = c0_; return ok ? i - 1 : -1; }
    return -1;
}

/* a ring the controller has surely left: never used; its last TRB written;
   or, on the same endpoint, the driver's ring moved on two TRBs from where I
   hung off (only through k + 1 itself, done by the controller) or a new ring.
   One left on another endpoint waits: nothing here tells me where that one
   is. */
static int um_free(struct um_ring *r, uint32_t slot, uint32_t dci, uint64_t seg, int k)
{
    if (!r->busy) return 1;
    volatile uint8_t *b = buf(r, r->cons);
    um_clean(b, UM_BUF);
    if (b[0] != UM_SENT || b[1] != UM_SENT || b[2] != UM_SENT || b[3] != UM_SENT) return 1;
    if (r->slot != slot || r->dci != dci) return 0;
    if (seg != r->seg) return 1;
    int d = (k - r->k + 255) % 255;
    return d >= 2 && d < 200;
}

static void tester(int on);

/* core 0, $5BA1 / $5BA0 (the mouse), $5BA3 / $5BA2 (the pad): the device's
   block at the address just written, or forgotten; $5BA5 / $5BA4 (n 2): the
   pad read for agaboot's Controller window, or no longer */
int aga_usb_command(int n, int on, uint32_t a)
{
    if (n == 2) { tester(on); return 1; }
    if (n != 0 && n != 1) return 1;
    struct um_dev *u = &devs[n];
    const char *name = dev_name[n];
    uint8_t was_addr = u->addr, was_dci = u->dci, was_root = u->root;
    uint32_t was_route = u->route;
    struct um_line at;
    if (!on) {
        if (u->have) um_log("[AGA] usb %s: forgotten", name);
        u->have = 0;
        return 1;
    }
    uint32_t len = n ? 34 : 20;
    if (a < 0x01000000u || (a & 3) || !mapped(a) || !mapped(a + len - 1)) {
        um_log("[AGA] usb %s: block at %08x refused (fast RAM only)", name, a);
        return 1;
    }
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)a;
    if (*(const volatile uint32_t *)p != (n ? UP_MAGIC : UM_MAGIC)) {
        um_log("[AGA] usb %s: block at %08x without its magic", name, a);
        return 1;
    }
    /* agaboot's longer block (usbmouse.c): flags bit 1 where the device is
       plugged in, at 20 (the mouse) or 36 (the pad); the pad's bit 2 its hat's
       first value at 11 and its sticks' ranges at 40 */
    uint8_t fl = n ? p[7] : p[11];
    if (fl & (n ? 6 : 2)) {
        len = n ? 56 : 24;
        if (!mapped(a + len - 1)) { um_log("[AGA] usb %s: block at %08x refused (fast RAM only)", name, a); return 1; }
    }
    u->root = 0; u->route = 0;
    if (fl & 2) {
        const volatile uint8_t *w = p + (n ? 36 : 20);
        u->root = w[0];
        u->route = (uint32_t)(w[1] << 16 | w[2] << 8 | w[3]) & 0xFFFFFu;
    }
    u->addr = p[4]; u->dci = p[5]; u->id = p[6];
    if (!n) {
        u->div = p[7] ? p[7] : 1;
        u->b_off = (uint16_t)(p[8] << 8 | p[9]); u->b_n = p[10]; u->flags = p[11];
        u->x_off = (uint16_t)(p[12] << 8 | p[13]); u->x_bits = p[14];
        u->y_off = (uint16_t)(p[16] << 8 | p[17]); u->y_bits = p[18];
    } else {
        u->flags = p[7];
        u->hat_off = (uint16_t)(p[8] << 8 | p[9]); u->hat_bits = p[10];
        u->sx_off = (uint16_t)(p[12] << 8 | p[13]); u->sx_bits = p[14];
        u->sy_off = (uint16_t)(p[16] << 8 | p[17]); u->sy_bits = p[18];
        for (int i = 0; i < 7; i++) u->btn[i] = (uint16_t)(p[20 + 2 * i] << 8 | p[21 + 2 * i]);
        u->hat_min = 0;
        u->sx_min = u->sx_max = u->sy_min = u->sy_max = 0;
        if (fl & 4) {
            const volatile uint8_t *r = p + 40;
            u->hat_min = p[11];
            u->sx_min = (int32_t)((uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3]);
            u->sx_max = (int32_t)((uint32_t)r[4] << 24 | r[5] << 16 | r[6] << 8 | r[7]);
            u->sy_min = (int32_t)((uint32_t)r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11]);
            u->sy_max = (int32_t)((uint32_t)r[12] << 24 | r[13] << 16 | r[14] << 8 | r[15]);
        }
    }
    if (live(n)) {                                 /* borrowed now: the endpoint stays as it is until it's back */
        u->addr = was_addr; u->dci = was_dci; u->root = was_root; u->route = was_route;
    }
    if (!um_alloc()) { u->have = 0; return 1; }
    if (!u->have || u->addr != was_addr || u->dci != was_dci || u->root != was_root || u->route != was_route) {
        if (!n)
            um_log("[AGA] usb mouse: %s DCI %u ID %u divisor %u, buttons %u+%u X %u+%u Y %u+%u",
                   um_at(u, &at), u->dci, u->id, u->div, u->b_off, u->b_n, u->x_off, u->x_bits, u->y_off, u->y_bits);
        else
            um_log("[AGA] usb pad: %s DCI %u ID %u, hat %u+%u X %u+%u Y %u+%u, buttons %d %d %d %d %d %d %d",
                   um_at(u, &at), u->dci, u->id, u->hat_off, u->hat_bits, u->sx_off, u->sx_bits, u->sy_off, u->sy_bits,
                   (int16_t)u->btn[0], (int16_t)u->btn[1], (int16_t)u->btn[2], (int16_t)u->btn[3],
                   (int16_t)u->btn[4], (int16_t)u->btn[5], (int16_t)u->btn[6]);
    }
    u->have = 1;
    return 1;
}

/* the onboard xHCI, mapped, or 0 if it's not playing */
static uintptr_t xhci_base(void)
{
    of_node_t *x = dt_find_node("/scb/xhci");
    of_property_t *st = x ? dt_find_property(x, "status") : 0;
    const char *status = st && st->op_value ? (const char *)st->op_value : "okay";
    if (!x || !(status[0] == 'o' && status[1] == 'k')) return 0;
    of_property_t *rg = dt_find_property(x, "reg");
    int ab = (int)dt_get_property_value_u32(dt_find_node("/scb"), "#address-cells", 1, 1);
    uint32_t bus = rg && ab >= 1 && rg->op_length >= 4u * (uint32_t)ab ? ((uint32_t *)rg->op_value)[ab - 1] : 0;
    uintptr_t base = bus ? scb_virt(bus) : 0;
    return base && mapped(base) ? base : 0;
}

/* the device's endpoint as the driver set it up: a running interrupt IN
   endpoint on a one-segment ring; 0 (said in the log) if it is not. The
   device is the slot with its USB address, or - found by agaboot through
   Poseidon, whose addresses are its own - the one plugged in where the block
   says: the slot context's root port (dword 1, bits 23-16) and route string
   (dword 0, bits 19-0) */
struct um_ep { uint32_t slot, dboff; uint64_t dcbaa_e, dc, seg; int ctx, maxp; };

static int find_ep(uintptr_t base, int n, struct um_ep *p)
{
    struct um_dev *u = &devs[n];
    const char *name = dev_name[n];
    struct um_line at;
    uint32_t caplen = mmio(base) & 0xFF;
    int ctx = (mmio(base + 0x10) & 4) ? 64 : 32;
    uintptr_t op = base + caplen;
    if (!(mmio(op) & 1) || (mmio(op + 4) & 1)) { um_log("[AGA] usb %s: the controller is not running", name); return 0; }
    uint64_t dcbaa = ((uint64_t)mmio(op + 0x34) << 32) | mmio(op + 0x30);
    int ok = 1;
    uint64_t dc = 0;
    uint32_t slot = 0;
    for (uint32_t s = 1; s <= (mmio(op + 0x38) & 0xFF) && s <= 64 && ok; s++) {
        uint64_t d = mem32(dcbaa + 8 * s, &ok) | (uint64_t)mem32(dcbaa + 8 * s + 4, &ok) << 32;
        d &= ~63ull;
        if (!d || !ok) continue;
        fresh(d, 32u * (uint32_t)ctx);
        uint32_t s0 = mem32(d, &ok), s1 = mem32(d + 4, &ok), s3 = mem32(d + 12, &ok);
        int hit = u->root ? (s0 & 0xFFFFFu) == u->route && ((s1 >> 16) & 0xFF) == u->root : (s3 & 0xFF) == u->addr;
        if (hit && ok) { dc = d; slot = s; break; }
    }
    if (!dc) { um_log("[AGA] usb %s: no device at %s", name, um_at(u, &at)); return 0; }
    uint64_t e = dc + (uint64_t)ctx * u->dci;
    uint32_t e0 = mem32(e, &ok), e1 = mem32(e + 4, &ok), e2 = mem32(e + 8, &ok), e3 = mem32(e + 12, &ok);
    if (!ok || ((e1 >> 3) & 7) != 7 || (e0 & 7) != 1) {
        um_log("[AGA] usb %s: slot %u DCI %u is no running interrupt IN endpoint (%08x %08x)", name, slot, u->dci, e0, e1);
        return 0;
    }
    int maxp = (int)(e1 >> 16);
    if (maxp < 1 || maxp > UM_BUF) { um_log("[AGA] usb %s: max packet %d", name, maxp); return 0; }
    uint64_t seg = (((uint64_t)e3 << 32) | (e2 & ~15u)) & ~0xFFFull;
    uint32_t l0 = mem32(seg + 255 * 16, &ok), l1 = mem32(seg + 255 * 16 + 4, &ok), lc = mem32(seg + 255 * 16 + 12, &ok);
    if (!ok || ((lc >> 10) & 63) != 6 || (((uint64_t)l1 << 32) | (l0 & ~15u)) != seg) {
        um_log("[AGA] usb %s: the driver's ring is not one segment", name);
        return 0;
    }
    p->slot = slot; p->dboff = mmio(base + 0x14) & ~3u;
    p->dcbaa_e = dcbaa + 8 * slot; p->dc = dc; p->seg = seg;
    p->ctx = ctx; p->maxp = maxp;
    return 1;
}

/* one device's detour in, if the driver left the endpoint as I expect it */
static void take(uintptr_t base, int n)
{
    struct um_dev *u = &devs[n];
    const char *name = dev_name[n];
    struct um_ep p;
    if (!find_ep(base, n, &p)) return;
    uint32_t slot = p.slot;
    uint64_t seg = p.seg;
    int maxp = p.maxp, ok = 1;
    for (int i = 0; i < UM_DEVS; i++)
        if (i != n && live(i) && devs[i].slot == slot && devs[i].dci == u->dci) {
            um_log("[AGA] usb %s: slot %u DCI %u is the %s's already", name, slot, u->dci, dev_name[i]);
            return;
        }
    int c, k = newest(seg, &c);
    if (k < 0) { um_log("[AGA] usb %s: the driver's next TRB is its link - not this game", name); return; }
    uint32_t kc = mem32(seg + 16u * k + 12, &ok);
    if (!ok || ((kc >> 10) & 63) != 1) { um_log("[AGA] usb %s: the driver's TRB %d is no transfer (%08x)", name, k, kc); return; }

    um_take();
    struct um_ring *r = 0;
    for (int i = 0; i < UM_RINGS && !r; i++)
        if (rings[i].mem && um_free(&rings[i], slot, u->dci, seg, k)) r = &rings[i];
    if (!r) { um_give(); um_log("[AGA] usb %s: every ring may still hold the controller - not this game", name); return; }
    /* mine: every TRB the last lap's (not the controller's), the first one given */
    for (int i = 0; i < UM_TRBS - 1; i++) {
        volatile uint8_t *b = buf(r, i);
        for (int j = 0; j < UM_BUF; j++) b[j] = UM_SENT;
        um_clean(b, UM_BUF);
        put_trb(trb(r, i), r->dma + 16 * UM_TRBS + UM_BUF * (uint64_t)i, (uint32_t)maxp, CTL_NORMAL(!c));
    }
    put_trb(trb(r, UM_TRBS - 1), r->dma, 0, CTL_LINK(!c, 1));
    trb(r, 0)[3] = LE32(CTL_NORMAL(c));
    um_clean(trb(r, 0), 16);
    /* k + 1: keep what's there, then my link, the driver's cycle */
    volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)(seg + 16u * (k + 1));
    for (int i = 0; i < 4; i++) u->saved_trb[i] = t[i];
    put_trb(t, r->dma, 0, CTL_LINK(c, 0));
    for (int i = 0; i < 4; i++) u->entry_trb[i] = t[i];
    u->db_reg = base + p.dboff + 4 * slot;
    *(volatile uint32_t *)u->db_reg = LE32(u->dci);
    __asm__ volatile("dsb sy" ::: "memory");
    u->cur = r; u->cons = 0; u->ccs = c; u->c0 = c; u->drv_seg = seg; u->drv_k = k; u->maxp = maxp; u->slot = slot;
    u->dcbaa_e = p.dcbaa_e; u->dc = p.dc; u->ctx = p.ctx;
    r->busy = 1; r->slot = slot; r->dci = u->dci; r->seg = seg; r->k = k; r->cons = 0;
    u->reports = u->logged = 0;
    u->last_b = 0;
    for (int i = 0; i < 16; i++) u->last[i] = UM_SENT;
    u->acc_x = u->acc_y = 0; u->cnt_x = u->cnt_y = 0;
    u->next_look = 0;
    if (!n) aga_pad_usb_mouse(TYPE_MOUSE << 28);
    else aga_pad_usb_pad(TYPE_GAMECTLR << 28);
    __atomic_or_fetch(&aga_usb_live, 1 << n, __ATOMIC_RELEASE);
    um_give();
    um_log("[AGA] usb %s: in - slot %u DCI %u, the driver's TRB %d (cycle %d), our ring %08x", name, slot, u->dci, k, c,
           (uint32_t)r->dma);
}

/* core 0: WHDLoad has the machine, so the detours go in */
void aga_usb_taken(void)
{
    if (__atomic_load_n(&um_tester, __ATOMIC_ACQUIRE)) {
        um_take();
        __atomic_store_n(&um_tester, 0, __ATOMIC_RELEASE);
        aga_pad_usb_pad(0);                        /* the window's last word belongs to no game - drop it */
        um_give();
        um_log("[AGA] usb pad: the Controller window's look ends - the game borrows it");
    }
    int want = 0;
    for (int n = 0; n < UM_DEVS; n++) want |= devs[n].have && !live(n);
    if (!want) return;
    uintptr_t base = xhci_base();
    if (!base) return;
    for (int n = 0; n < UM_DEVS; n++)
        if (devs[n].have && !live(n)) take(base, n);
}

static int32_t field(const uint8_t *rep, unsigned off, unsigned n, int sgn)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n && i < 32; i++) {
        unsigned b = off + i;
        if (b >= 8u * UM_BUF) break;
        v |= (uint32_t)((rep[b >> 3] >> (b & 7)) & 1u) << i;
    }
    if (sgn && n > 0 && n < 32 && ((v >> (n - 1)) & 1)) v |= ~0u << n;
    return (int32_t)v;
}

/* counts to the Amiga's: through the divisor, the rest kept for next time,
   at most 40 a report, no exceptions. */
static void move(struct um_dev *u, int32_t d, int32_t *acc, uint8_t *cnt)
{
    *acc += d;
    int32_t s = *acc / u->div;
    *acc -= s * u->div;
    if (s > 40) s = 40;
    if (s < -40) s = -40;
    *cnt = (uint8_t)(*cnt + s);
}

static void log_report(int n, const uint8_t *rep)
{
    um_log("[AGA] usb %s: report %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", dev_name[n], rep[0],
           rep[1], rep[2], rep[3], rep[4], rep[5], rep[6], rep[7], rep[8], rep[9], rep[10], rep[11]);
}

static void mouse_report(struct um_dev *u, const uint8_t *rep)
{
    uint32_t b = (uint32_t)field(rep, u->b_off, u->b_n, 0);
    move(u, field(rep, u->x_off, u->x_bits, 1), &u->acc_x, &u->cnt_x);
    move(u, field(rep, u->y_off, u->y_bits, 1), &u->acc_y, &u->cnt_y);
    if ((u->flags & 1) && u->logged < 48 && (u->logged < 16 || b != u->last_b)) {
        u->logged++;
        log_report(0, rep);
    }
    u->last_b = b;
    aga_pad_usb_mouse((TYPE_MOUSE << 28) | ((b & 1) ? JPF_RED : 0u) | ((b & 2) ? JPF_BLUE : 0u)
                      | ((b & 4) ? JPF_PLAY : 0u) | (uint32_t)u->cnt_y << 8 | u->cnt_x);
}

/* a stick's direction: the outer quarters of its range - the logical one
   the block gives (signed when it starts below 0), or else all its bits,
   unsigned. Same rule either way, thank Christ. */
static uint32_t stick(const uint8_t *rep, unsigned off, unsigned bits, int32_t mn, int32_t mx, uint32_t lo, uint32_t hi)
{
    if (bits < 2 || bits > 16) return 0;
    if (mn < mx) {
        int32_t v = field(rep, off, bits, mn < 0), q = (mx - mn) / 4;
        return v < mn + q ? lo : v > mx - q ? hi : 0;
    }
    uint32_t v = (uint32_t)field(rep, off, bits, 0), q = 1u << (bits - 2);
    return v < q ? lo : v >= 3 * q ? hi : 0;
}

static void pad_report(struct um_dev *u, const uint8_t *rep)
{
    uint32_t w = TYPE_GAMECTLR << 28;
    if (u->hat_bits) {
        uint32_t h = (uint32_t)field(rep, u->hat_off, u->hat_bits, 0) - u->hat_min;   /* up: 0, or 1 on some pads that like to be difficult */
        if (h < 8) w |= hat_dir[h];
    }
    w |= stick(rep, u->sx_off, u->sx_bits, u->sx_min, u->sx_max, JPF_LEFT, JPF_RIGHT);
    w |= stick(rep, u->sy_off, u->sy_bits, u->sy_min, u->sy_max, JPF_UP, JPF_DOWN);
    for (int i = 0; i < 7; i++)
        if (u->btn[i] != 0xFFFF && field(rep, u->btn[i], 1, 0)) w |= pad_jpf[i];
    int changed = 0;
    for (int i = 0; i < 16 && i < u->maxp; i++) changed |= rep[i] != u->last[i];
    if ((u->flags & 1) && changed && u->logged < 96) {
        u->logged++;
        log_report(1, rep);
    }
    for (int i = 0; i < 16; i++) u->last[i] = rep[i];
    aga_pad_usb_pad(w);
}

/* one visit to a device's given TRB: a report in its buffer, the next TRB
   given. Nothing clever, just get in and get out. */
static void look(struct um_dev *u, int n)
{
    volatile uint8_t *b = buf(u->cur, u->cons);
    um_clean(b, UM_BUF);
    int got = 0;
    for (int i = 0; i < 16 && !got; i++) got = b[i] != UM_SENT;
    if (!got) return;
    uint8_t rep[UM_BUF];
    for (int i = 0; i < UM_BUF; i++) { rep[i] = b[i]; b[i] = UM_SENT; }
    um_clean(b, UM_BUF);
    /* the next TRB is the controller's; past the last, the first of the next lap */
    if (u->cons + 1 == UM_TRBS - 1) {
        trb(u->cur, 0)[3] = LE32(CTL_NORMAL(!u->ccs));
        um_clean(trb(u->cur, 0), 16);
        trb(u->cur, UM_TRBS - 1)[3] = LE32(CTL_LINK(u->ccs, 1));
        um_clean(trb(u->cur, UM_TRBS - 1), 16);
        u->cons = 0;
        u->ccs = !u->ccs;
    } else {
        u->cons++;
        trb(u->cur, u->cons)[3] = LE32(CTL_NORMAL(u->ccs));
        um_clean(trb(u->cur, u->cons), 16);
    }
    *(volatile uint32_t *)u->db_reg = LE32(u->dci);
    __asm__ volatile("dsb sy" ::: "memory");
    u->reports++;
    if (u->id && rep[0] != u->id) return;          /* another report of the same interface - bin it */
    if (!n) mouse_report(u, rep);
    else pad_report(u, rep);
}

/* the peek's transfer: the driver's TRB drv_k, queued in lap ccs - its
   buffer watched from what it holds now, or none */
static void watch(struct um_dev *u)
{
    int ok = 1;
    uint64_t t = u->drv_seg + 16u * (uint32_t)u->drv_k;
    uint32_t ctl = mem32(t + 12, &ok), len = mem32(t + 8, &ok) & 0x1FFFF;
    uint64_t a = mem32(t, &ok) | (uint64_t)mem32(t + 4, &ok) << 32;
    u->pk_len = 0;
    if (!ok || (int)(ctl & 1) != u->ccs || ((ctl >> 10) & 63) != 1 || !len || a >= (1ull << 40)
        || !mapped((uintptr_t)a) || !mapped((uintptr_t)(a + len - 1))) return;
    if (len > UM_BUF) len = UM_BUF;
    fresh(a, len);
    for (uint32_t i = 0; i < len; i++) u->pk_was[i] = *(volatile uint8_t *)(uintptr_t)(a + i);
    u->pk_a = a;
    u->pk_len = len;
}

/* one visit to the driver's ring for the Controller window's tester (the
   header says why so often): a newer transfer queued means the one watched
   is done - the next one watched from now on; else the watched buffer
   changed is the report. Only read - the TRBs are the CPU's, coherent; the
   buffer the controller writes is read past the cache. Not a pretty scheme,
   but it keeps Poseidon alive. */
static void peek(struct um_dev *u, int n)
{
    int ok = 1, k = u->drv_k, c = u->ccs, moved = 0;
    for (int i = 0; i < 32; i++) {
        int nk = k + 1, nc = c;
        if (nk == 255) { nk = 0; nc = !c; }          /* past its link: on to the next lap */
        uint32_t ctl = mem32(u->drv_seg + 16u * (uint32_t)nk + 12, &ok), type = (ctl >> 10) & 63;
        if (!ok || (int)(ctl & 1) != nc || (type != 1 && type != 8)) break;   /* a transfer, or one cancelled (a no-op) */
        k = nk; c = nc; moved = 1;
    }
    if (moved) {
        if (u->pk_len) u->missed++;                /* done before a visit caught the controller's write */
        u->drv_k = k; u->ccs = c;
        watch(u);
        return;
    }
    if (!u->pk_len) return;
    uint32_t len = u->pk_len;
    uint8_t rep[UM_BUF];
    fresh(u->pk_a, len);
    int changed = 0;
    for (uint32_t i = 0; i < UM_BUF; i++) {
        rep[i] = i < len ? *(volatile uint8_t *)(uintptr_t)(u->pk_a + i) : 0;
        if (i < len) changed |= rep[i] != u->pk_was[i];
    }
    if (!changed) return;
    u->pk_len = 0;                                 /* once only: what the driver writes there next is not a report */
    u->reports++;
    if (u->id && rep[0] != u->id) return;          /* another report of the same interface - bin it */
    if (!n) mouse_report(u, rep);
    else pad_report(u, rep);
}

/* the housekeeper core, each visit: every 0.5 ms a look at each device's
   given TRB while its detour is in, or at every visit at the driver's ring
   while the Controller window reads it. Tight, but it has to be. */
void aga_usb_service(void)
{
    int tw = __atomic_load_n(&um_tester, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&aga_usb_live, __ATOMIC_ACQUIRE) && !tw) return;
    uint64_t now, f;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    for (int n = 0; n < UM_DEVS; n++) {
        struct um_dev *u = &devs[n];
        if (!(live(n) || ((tw >> n) & 1)) || now < u->next_look) continue;
        u->next_look = live(n) ? now + f / 2000u : 0;   /* the peek runs at every visit */
        if (!um_try()) return;
        if (live(n)) look(u, n);
        else if ((__atomic_load_n(&um_tester, __ATOMIC_ACQUIRE) >> n) & 1) peek(u, n);
        um_give();
    }
}

/* one device's detour out, um_lock held: back to the driver's ring - unless
   the driver dropped the device meanwhile, its ring maybe free memory now.
   Then I keep my hands to myself. */
static void give_back(int n)
{
    struct um_dev *u = &devs[n];
    int ok = 1, r = u->cons + 1, ours = 0, turned = 0;   /* r: never a TRB the controller owns */
    uint64_t d = mem32(u->dcbaa_e, &ok) | (uint64_t)mem32(u->dcbaa_e + 4, &ok) << 32;
    int gone = !ok || (d & ~63ull) != u->dc;
    if (!gone) {
        uint64_t e = u->dc + (uint64_t)u->ctx * u->dci;
        fresh(e, 4);
        gone = (mem32(e, &ok) & 7) == 0 || !ok;
    }
    if (!gone) {
        put_trb(trb(u->cur, r), u->drv_seg + 16u * (u->drv_k + 1), 0, CTL_LINK(u->ccs, u->ccs != u->c0));
        volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)(u->drv_seg + 16u * (u->drv_k + 1));
        fresh((uint64_t)(uintptr_t)t, 16);
        ours = t[0] == u->entry_trb[0] && t[1] == u->entry_trb[1] && t[2] == u->entry_trb[2] && t[3] == u->entry_trb[3];
        /* my Link with just its cycle turned: the driver ran and queued past
           it (the header says how) - its next transfers wait at TRB 0 */
        turned = !ours && t[0] == u->entry_trb[0] && t[1] == u->entry_trb[1] && t[2] == u->entry_trb[2]
                 && (t[3] ^ u->entry_trb[3]) == LE32(1u);
        if (ours) {
            t[0] = u->saved_trb[0]; t[1] = u->saved_trb[1]; t[2] = u->saved_trb[2];
            um_clean(t, 16);
            t[3] = u->saved_trb[3];
            um_clean(t, 16);
        }
        *(volatile uint32_t *)u->db_reg = LE32(u->dci);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    u->cur->cons = u->cons;
    __atomic_and_fetch(&aga_usb_live, ~(1 << n), __ATOMIC_RELEASE);
    if (!n) aga_pad_usb_mouse(0);
    else aga_pad_usb_pad(0);
    if (gone)
        um_log("[AGA] usb %s: out - %u reports; the driver had dropped it, its ring not touched", dev_name[n], u->reports);
    else
        um_log("[AGA] usb %s: out - %u reports, back to the driver's TRB %d from ours %d%s", dev_name[n], u->reports,
               u->drv_k + 1, r, ours ? ", its slot put back"
               : turned ? " - but the driver queued past our Link meanwhile: plug it in again for Poseidon" : "");
}

/* core 0, at the leave (and a game inside System wide ending): back to the
   drivers' rings before their held interrupt reaches the driver */
void aga_usb_leave(void)
{
    if (!__atomic_load_n(&aga_usb_live, __ATOMIC_ACQUIRE)) return;
    um_take();
    for (int n = 0; n < UM_DEVS; n++)
        if (live(n)) give_back(n);
    um_give();
}

/* core 0, $5BA5 / $5BA4: the pad read for agaboot's Controller window, or
   no longer - outside a game, its driver running, so only read (peek; the
   header says why). A game's borrow is left alone, and a good thing too. */
static void tester(int on)
{
    struct um_dev *u = &devs[1];
    if (!on) {
        um_take();
        int was = __atomic_fetch_and(&um_tester, ~2, __ATOMIC_ACQ_REL) & 2;
        if (was && !live(1)) aga_pad_usb_pad(0);
        um_give();
        if (was) um_log("[AGA] usb pad: the Controller window's look ends - %u reports, %u missed", u->reports, u->missed);
        return;
    }
    if (!u->have || live(1) || (__atomic_load_n(&um_tester, __ATOMIC_ACQUIRE) & 2)) return;
    uintptr_t base = xhci_base();
    struct um_ep p;
    if (!base || !find_ep(base, 1, &p)) return;
    int ok = 1, c, k = newest(p.seg, &c);
    if (k < 0) {                                   /* TRB 254 is the newest, or the ring is unreadable */
        c = (int)(mem32(p.seg + 12, &ok) & 1);
        k = 254;
        if (!ok) return;
    }
    um_take();
    u->drv_seg = p.seg; u->drv_k = k; u->ccs = c; u->maxp = p.maxp; u->slot = p.slot;
    u->reports = u->logged = u->missed = 0;
    for (int i = 0; i < 16; i++) u->last[i] = UM_SENT;
    watch(u);                                      /* the transfer queued right now */
    u->next_look = 0;
    aga_pad_usb_pad(TYPE_GAMECTLR << 28);
    __atomic_or_fetch(&um_tester, 2, __ATOMIC_RELEASE);
    um_give();
    um_log("[AGA] usb pad: read for the Controller window - slot %u DCI %u, the driver's TRB %d (cycle %d)", p.slot,
           u->dci, k, c);
}

static void um_say(void)
{
    for (int n = 0; n < UM_DEVS; n++) {
        struct um_dev *u = &devs[n];
        struct um_line at;
        if (!u->have) { say("%s in games: none from agaboot (agaboot USBFIND)", dev_name[n]); continue; }
        if (!n)
            say("mouse in games: %s DCI %u ID %u divisor %u, buttons %u+%u X %u+%u Y %u+%u; %s, %u reports",
                um_at(u, &at), u->dci, u->id, u->div, u->b_off, u->b_n, u->x_off, u->x_bits, u->y_off, u->y_bits,
                live(n) ? "in a game now" : "not in a game", u->reports);
        else if ((um_tester >> n) & 1)
            say("pad: read for agaboot's Controller window, %u reports, %u missed", u->reports, u->missed);
        else
            say("pad in games: %s DCI %u ID %u, hat %u+%u X %u+%u Y %u+%u; %s, %u reports",
                um_at(u, &at), u->dci, u->id, u->hat_off, u->hat_bits, u->sx_off, u->sx_bits, u->sy_off, u->sy_bits,
                live(n) ? "in a game now" : "not in a game", u->reports);
    }
    for (int i = 0; i < UM_RINGS; i++) {
        struct um_ring *r = &rings[i];
        if (!r->mem) continue;
        int in = 0;
        for (int n = 0; n < UM_DEVS; n++) in |= live(n) && devs[n].cur == r;
        volatile uint8_t *b = buf(r, r->cons);
        um_clean(b, UM_BUF);
        int left = b[0] != UM_SENT || b[1] != UM_SENT || b[2] != UM_SENT || b[3] != UM_SENT;
        say("    ring %d at %08x: %s, TRB %d", i, (uint32_t)r->dma,
            !r->busy ? "unused" : in ? "in use" : left ? "used, left" : "used, its last TRB still given", r->cons);
    }
}

/* the 68k's RESET: the next OS's driver resets the controller itself, so I don't */
void aga_usb_reset(void)
{
    um_take();
    __atomic_store_n(&aga_usb_live, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&um_tester, 0, __ATOMIC_RELEASE);
    aga_pad_usb_mouse(0);
    aga_pad_usb_pad(0);
    for (int i = 0; i < UM_RINGS; i++) rings[i].busy = 0;
    um_give();
}

/* aga_diag.c, selector $1806: the first word read runs the look; then the
   text, two characters a word, 0 after its end */
uint16_t aga_usb_probe_word(uint32_t pos)
{
    if (pos == 0) probe();
    uint32_t i = pos * 2;
    uint8_t a = i < out_len ? (uint8_t)out[i] : 0, b = i + 1 < out_len ? (uint8_t)out[i + 1] : 0;
    return (uint16_t)((a << 8) | b);
}
