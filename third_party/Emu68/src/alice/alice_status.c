/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68's status page (alice_status.h): the kernel's side. The Emu68 ROM board asks for the
 * page when Kickstart configures it (alice_status_page) and maps it read-only into its own
 * Zorro III space; CPU1 fills it about once a second from its loop (alice_status_poll, called
 * by alice_net_poll) and times that loop for its own busy figure (alice_status_loop).
 *
 * Honest numbers only: CPU0 runs the 68k's code without a moment the kernel could call idle,
 * CPU2 and CPU3 run the display and the chipset without a cheap point to time - those show
 * ALICE68_ST_UNKNOWN, as do counters nothing keeps (USB bytes, SD blocks, JIT flushes/hits).
 *
 * Temperature: the BCM2711's AVS monitor register (as Linux's bcm2711_thermal reads it: the
 * code in bits 0-9, valid while bit 10 or 16 is set, mC = 410040 - 487 * code), no mailbox.
 * ARM clock, core voltage and the throttled bits need the VideoCore mailbox, whose answers come
 * back on one channel for everyone: a kernel call can take the answer a 68k program (the RTG
 * driver, a monitor) waits for. So they are read only with status.sensors=mbox in cmdline.txt,
 * every 5 s, and skipped while the WiFi coroutine has the mailbox.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "devicetree.h"
#include "M68k.h"
#include "alice_net.h"
#include "alice_status.h"

extern void *tlsf;
extern const char _verstring_object[];
extern struct M68KState *__m68k_state;
extern volatile uint8_t *alice_net_board(void);
extern const char *alice_bootargs(void);
extern int alice_mbox2(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2]);

static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

static volatile struct alice68_status *st;      /* the page, kernel side; 0 until the ROM board is mapped */
static uint32_t mbox_sensors = 2;               /* 2: not looked yet */
static uint64_t next_update, next_sensors;
static volatile uint32_t *avs;                  /* the AVS temperature register, 0: not found */
static int avs_looked;

/* the Emu68 ROM board, on CPU0 while Kickstart configures it: one zeroed 4 KB page */
void *alice_status_page(void)
{
    if (!st) {
        void *p = tlsf_malloc_aligned(tlsf, 4096, 4096);
        if (!p) return 0;
        uint8_t *b = p;
        for (int i = 0; i < 4096; i++) b[i] = 0;
        volatile struct alice68_status *s = p;
        s->magic = ALICE68_STATUS_MAGIC;
        s->version = ALICE68_STATUS_VERSION;
        s->size = sizeof(struct alice68_status);
        __asm__ volatile("dmb ish" ::: "memory");
        st = s;
    }
    return (void *)st;
}

/* ---- CPU1's own load: the loop's time beyond its shortest turn ---------------------------- */
static uint64_t loop_last, loop_min, loop_extra, loop_t0;
void alice_status_loop(void)
{
    uint64_t now = ticks();
    if (loop_last) {
        uint64_t dt = now - loop_last;
        if (!loop_min || dt < loop_min) loop_min = dt;
        loop_extra += dt - loop_min;            /* an empty turn costs loop_min; the rest was work */
    } else loop_t0 = now;
    loop_last = now;
}
static uint32_t loop_busy(uint64_t now)
{
    uint64_t span = now - loop_t0;
    if (!loop_t0 || !span) return ALICE68_ST_UNKNOWN;
    uint64_t b = loop_extra * 10000u / span;
    loop_t0 = now; loop_extra = 0; loop_min = 0;   /* the next window learns its own shortest turn */
    return b > 10000u ? 10000u : (uint32_t)b;
}

/* a bus address (0x7exxxxxx...) to the kernel's mapping, through /soc's ranges (start_rpi64.c
   rewrites their CPU side to where it mapped them) */
static volatile uint32_t *soc_reg(uint32_t bus)
{
    of_node_t *e = dt_find_node("/soc");
    of_property_t *p = e ? dt_find_property(e, "ranges") : 0;
    if (!p) return 0;
    int acpu = dt_get_property_value_u32(e->on_parent, "#address-cells", 1, 0);
    int abus = dt_get_property_value_u32(e, "#address-cells", 1, 1);
    int sbus = dt_get_property_value_u32(e, "#size-cells", 1, 1);
    const uint32_t *r = p->op_value;
    int n = abus + acpu + sbus;
    for (int32_t len = p->op_length; len >= 4 * n; len -= 4 * n, r += n) {
        uint32_t b = BE32(r[abus - 1]), c = BE32(r[abus + acpu - 1]), sz = BE32(r[n - 1]);
        if (bus >= b && bus - b < sz) return (volatile uint32_t *)(uintptr_t)(c + (bus - b));
    }
    return 0;
}
static uint32_t temperature(void)
{
    if (!avs_looked) { avs_looked = 1; avs = soc_reg(0x7d5d2200u); }   /* AVS_RO_TEMP_STATUS */
    if (!avs) return ALICE68_ST_UNKNOWN;
    uint32_t v = __builtin_bswap32(*avs);
    if (!(v & ((1u << 16) | (1u << 10)))) return ALICE68_ST_UNKNOWN;
    int32_t mc = 410040 - 487 * (int32_t)(v & 0x3ffu);
    return mc < 0 ? 0 : (uint32_t)mc;
}

static int word_in(const char *s, const char *w)
{
    unsigned n = 0;
    while (w[n]) n++;
    for (const char *p = s; p && *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        unsigned i = 0;
        while (i < n && p[i] == w[i]) i++;
        if (i == n && (!p[n] || p[n] == ' ' || p[n] == '\n')) return 1;
    }
    return 0;
}
static void copy_str(volatile char *d, const char *s, unsigned max)
{
    unsigned k = 0;
    while (s && s[k] && k + 1 < max) { d[k] = s[k]; k++; }
    while (k < max) d[k++] = 0;
}

/* CPU1, from alice_net_poll: the page again, about once a second */
void alice_status_poll(void)
{
    volatile struct alice68_status *s = st;
    if (!s) return;
    uint64_t now = ticks(), hz = tick_hz();
    if ((int64_t)(now - next_update) < 0) return;
    next_update = now + hz;
    uint32_t up = (uint32_t)(now / (hz / 1000u));
    if (mbox_sensors == 2) mbox_sensors = (uint32_t)word_in(alice_bootargs(), "status.sensors=mbox");

    s->seq++;                                   /* odd: being written */
    __asm__ volatile("dmb ish" ::: "memory");
    if (!s->build[0]) {
        const char *v = _verstring_object;
        if (v[0] == '$' && v[1] == 'V') v += 6;   /* "$VER: " */
        copy_str(s->build, v, sizeof s->build);
        static const char *const names[4] = { "68k JIT", "USB, network", "display", "chipset, audio" };
        static const uint32_t roles[4] = { ALICE68_ROLE_JIT, ALICE68_ROLE_IO, ALICE68_ROLE_DISPLAY, ALICE68_ROLE_CHIPSET };
        for (int i = 0; i < 4; i++) { s->core[i].role = roles[i]; copy_str(s->core[i].name, names[i], sizeof s->core[i].name); }
        s->jit_flushes = s->jit_hits = ALICE68_ST_UNKNOWN;
        s->usb_in_bytes = s->usb_out_bytes = ALICE68_ST_UNKNOWN;
        s->sd_read_blocks = s->sd_write_blocks = ALICE68_ST_UNKNOWN;
        s->arm_hz = s->core_uv = s->throttled = ALICE68_ST_UNKNOWN;
    }
    s->uptime_ms = up;
    s->interval_ms = 1000;
    s->flags = mbox_sensors ? ALICE68_STF_MBOX_SENSORS : 0;
    s->core[0].busy = ALICE68_ST_UNKNOWN;
    s->core[1].busy = loop_busy(now);
    s->core[2].busy = ALICE68_ST_UNKNOWN;
    s->core[3].busy = ALICE68_ST_UNKNOWN;
    s->temp_mc = temperature();
    if (mbox_sensors && (int64_t)(now - next_sensors) >= 0) {
        next_sensors = now + 5 * hz;
        uint32_t o[2];
        int ok = 1;
        if (alice_mbox2(0x00030002u, 3u, 0u, o) == 0) s->arm_hz = o[1]; else ok = 0;      /* GET_CLOCK_RATE: ARM */
        if (ok && alice_mbox2(0x00030003u, 1u, 0u, o) == 0) s->core_uv = o[1]; else ok = 0;   /* GET_VOLTAGE: core */
        if (ok && alice_mbox2(0x00030046u, 0u, 0u, o) == 0) s->throttled = o[0]; else ok = 0; /* GET_THROTTLED */
        if (ok) s->sensors_ms = up;
    }
    struct M68KState *m = __m68k_state;
    if (m) {
        s->jit_total = m->JIT_CACHE_TOTAL;
        s->jit_free = m->JIT_CACHE_FREE;
        s->jit_units = m->JIT_UNIT_COUNT;
        s->jit_translations = m->JIT_CACHE_MISS;
    }
    volatile uint8_t *b = alice_net_board();
    for (int u = 0; u < 2; u++) {
        volatile struct alice68_status_net *n = &s->net[u];
        if (!b) { n->kind = 0; continue; }
        volatile struct an_unit *x = (volatile struct an_unit *)(b + AN_OFF_UNIT(u));
        n->kind = x->kind; n->link = x->link;
        n->rx_pkts = x->rx_pkts; n->tx_pkts = x->tx_pkts; n->rx_bytes = x->rx_bytes; n->tx_bytes = x->tx_bytes;
        n->rx_drop = x->rx_drop; n->tx_err = x->tx_err;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    s->seq++;                                   /* even: done */
}
