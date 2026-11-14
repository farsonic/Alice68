/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 - the Raspberry Pi 4's USB-A ports for the virtual Amiga.
 *
 * The Pi 4's four USB-A ports (and the Pi 400's keyboard) sit behind a VIA
 * VL805 xHCI controller on the BCM2711's PCIe link. This drives it from the
 * kernel, on CPU1, polled - no interrupts - so a USB keyboard and mouse work
 * from power-on, in Kickstart's boot menu, on Workbench and inside WHDLoad
 * games, without any AmigaOS driver:
 *
 *   PCIe root complex up (the sequence of rondoval's bcmpcie.library /
 *   U-Boot's pcie_brcmstb.c), the VL805's BAR placed in the outbound window,
 *   its firmware reloaded by the VideoCore (mailbox tag 0x30058), xHCI reset,
 *   command and event rings, every connected root port reset and addressed,
 *   HID boot-protocol keyboards and mice configured, their interrupt IN
 *   reports fed to the virtual CIA keyboard line and the mouse port exactly
 *   as the Pi 3 DWC2 host does (frf_native_usb_keyboard_boot_report,
 *   aga_virtual_mouse_update). Plug and unplug are followed.
 *
 * Phase 1: everything that is not a boot keyboard or mouse is left
 * unconfigured. Hubs are not followed yet. With frf_usb=neptune in
 * cmdline.txt this file does nothing and the controller stays for
 * rondoval's xhci.device + Neptune in AmigaOS - the two cannot share it.
 *
 * All memory the controller reads or writes lives in a 2 MB pool below 1 GB
 * (mmu.c), mapped 1:1 and uncached (start.c): bus address == CPU address,
 * no cache maintenance. Everything the VL805 sees is little-endian; this
 * CPU runs big-endian, hence LE32 on every access.
 */
#include <stdint.h>
#include <stddef.h>
#include "devicetree.h"
#include "support.h"
#include <stdarg.h>
#include "boards.h"

/* every [FRF-XHCI] line: the boot log (screen, serial); kprintf also puts
   every line in the pass-through board's log ring (Alice68-USBLog, Alice68-Monitor) */
typedef void (*xlog_putc_t)(void *data, char c);
extern void vkprintf_pc(xlog_putc_t putc_f, void *putc_data, const char *format, va_list args);
struct xlog_buf { char s[240]; unsigned n; };
static void xlog_c(void *d, char c) { struct xlog_buf *b = d; if (b->n < sizeof b->s - 1) b->s[b->n++] = c; }
static void xlog(const char *fmt, ...)
{
    struct xlog_buf b; b.n = 0;
    va_list ap; va_start(ap, fmt); vkprintf_pc(xlog_c, &b, fmt, ap); va_end(ap);
    b.s[b.n] = 0;
    kprintf("%s", b.s);                     /* kprintf puts it in the ring too (alice_klog_putc) */
}

#define LE32(x) __builtin_bswap32((uint32_t)(x))

extern uintptr_t frf_dma_phys;
extern volatile uint8_t frf_usb_boot_ready;
extern void frf_boot_stage(unsigned stage);
extern void frf_post_usb_devices(unsigned kb, unsigned ms, unsigned gp, unsigned msc);
extern void frf_native_usb_keyboard_boot_report(const uint8_t report[8]);
extern void frf_native_usb_keyboard_force_release(void);
extern int  frf_native_usb_reboot_requested(void);
extern int  frf_native_usb_amiga_reset_requested(void);
extern void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);
extern void aga_reboot(int tryboot);
extern volatile uint8_t frf_usb_mouse_seen, frf_usb_mouse_buttons;

/* ---- time ------------------------------------------------------------------ */
static inline uint64_t now_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void)   { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static void udelay(uint32_t us)
{
    uint64_t end = now_ticks() + tick_hz() * us / 1000000u;
    while ((int64_t)(now_ticks() - end) < 0) __asm__ volatile("yield");
}
static void mdelay(uint32_t ms) { udelay(ms * 1000u); }

/* ---- MMIO (little-endian devices) ------------------------------------------ */
static inline uint32_t rd32(uintptr_t a) { uint32_t v = *(volatile uint32_t *)a; __asm__ volatile("dmb oshld" ::: "memory"); return LE32(v); }
static inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("dmb oshst" ::: "memory"); *(volatile uint32_t *)a = LE32(v); __asm__ volatile("dsb oshst" ::: "memory"); }
static inline uint16_t rd16(uintptr_t a) { uint16_t v = *(volatile uint16_t *)a; __asm__ volatile("dmb oshld" ::: "memory"); return __builtin_bswap16(v); }
static inline void wr64(uintptr_t a, uint64_t v) { wr32(a, (uint32_t)v); wr32(a + 4, (uint32_t)(v >> 32)); }

/* ==== PCIe root complex (BCM2711 brcmstb) ==================================== */
#define PCIE_RC_CFG_VENDOR_SPECIFIC_REG1   0x0188
#define PCIE_RC_CFG_PRIV1_ID_VAL3          0x043c
#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY  0x04dc
#define PCIE_MISC_MISC_CTRL                0x4008
#define PCIE_MEM_WIN0_LO                   0x400c
#define PCIE_MEM_WIN0_HI                   0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO        0x402c
#define PCIE_MISC_RC_BAR2_CONFIG_LO        0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI        0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO        0x403c
#define PCIE_MISC_PCIE_STATUS              0x4068
#define PCIE_MISC_REVISION                 0x406c
#define PCIE_MEM_WIN0_BASE_LIMIT           0x4070
#define PCIE_MEM_WIN0_BASE_HI              0x4080
#define PCIE_MEM_WIN0_LIMIT_HI             0x4084
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG     0x4204
#define PCIE_MSI_INTR2_CLR                 0x4508
#define PCIE_MSI_INTR2_MASK_SET            0x4510
#define PCIE_EXT_CFG_DATA                  0x8000
#define PCIE_EXT_CFG_INDEX                 0x9000
#define PCIE_RGR1_SW_INIT_1                0x9210

static uintptr_t rc;          /* RC registers, kernel virtual */
static uintptr_t mmio_virt;   /* outbound window, kernel virtual */
static uint32_t  mmio_pci;    /* ...its PCI bus address */
static uint64_t  mmio_cpu;    /* ...its CPU physical address */
static uint32_t  mmio_size;

static int link_up(void)
{
    uint32_t v = rd32(rc + PCIE_MISC_PCIE_STATUS);
    return (v & (1u << 5)) && (v & (1u << 4));      /* DL active, PHY link up */
}

static uintptr_t cfg_addr(int bus, int dev, int off)
{
    if (bus == 0) return rc + off;
    wr32(rc + PCIE_EXT_CFG_INDEX, ((uint32_t)bus << 20) | ((uint32_t)dev << 15));
    return rc + PCIE_EXT_CFG_DATA + off;
}
static uint32_t cfg_rd(int bus, int off)              { return rd32(cfg_addr(bus, 0, off)); }
static void     cfg_wr(int bus, int off, uint32_t v)  { wr32(cfg_addr(bus, 0, off), v); }

/* /scb child bus address -> kernel virtual (Emu68 rewrote /scb ranges to its
   0xF2xxxxxx mappings, start_rpi64.c map_peripheral_ranges) */
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
        uint32_t b = r[ab - 1], c = r[ab + ac - 1], l = r[n - 1];   /* big-endian cells, read natively */
        if (b == c) continue;
        if (bus - b < l) return (uintptr_t)c + (bus - b);
    }
    return 0;
}

static of_node_t *pcie_node(void)
{
    of_node_t *al = dt_find_node("/aliases");
    of_property_t *ap = al ? dt_find_property(al, "pcie0") : 0;
    return (ap && ap->op_value) ? dt_find_node((char *)ap->op_value) : 0;
}

static int pcie_find(void)
{
    of_node_t *n = pcie_node();
    if (!n) { xlog("[FRF-XHCI] no pcie0 in the device tree - not a Pi 4/CM4?\n"); return -1; }
    of_property_t *reg = dt_find_property(n, "reg");
    of_property_t *pv  = dt_find_property(n, "emu68,pci-mmio-virt");
    of_property_t *rg  = dt_find_property(n, "ranges");
    if (!reg || !pv || !rg) { xlog("[FRF-XHCI] pcie0: reg/ranges/emu68,pci-mmio-virt missing\n"); return -1; }
    int ac = (int)dt_get_property_value_u32(n->on_parent, "#address-cells", 2, 0);
    uint32_t *r = reg->op_value;
    rc = scb_virt(r[ac - 1]);
    mmio_virt = ((uint32_t *)pv->op_value)[0];
    uint32_t *g = rg->op_value;               /* first range: <space pci_hi pci_lo> <cpu..> <size..> */
    int cpu_cells = (int)dt_get_property_value_u32(n->on_parent, "#address-cells", 2, 0);
    mmio_pci = g[2];
    mmio_cpu = cpu_cells >= 2 ? (((uint64_t)g[3] << 32) | g[4]) : g[3];
    mmio_size = 0x04000000u;                  /* Emu68 clamps the window to 64 MB */
    xlog("[FRF-XHCI] pcie0: RC regs virt %p, MMIO pci %08x cpu %08x%08x virt %08x\n",
            (void *)rc, mmio_pci, (uint32_t)(mmio_cpu >> 32), (uint32_t)mmio_cpu, (uint32_t)mmio_virt);
    return rc && mmio_virt ? 0 : -1;
}

static int pcie_bringup(void)
{
    uint32_t v;
    /* bridge + fundamental reset, then bridge out of reset, SerDes on */
    wr32(rc + PCIE_RGR1_SW_INIT_1, rd32(rc + PCIE_RGR1_SW_INIT_1) | 3u);
    udelay(100);
    wr32(rc + PCIE_RGR1_SW_INIT_1, rd32(rc + PCIE_RGR1_SW_INIT_1) & ~2u);
    wr32(rc + PCIE_MISC_HARD_PCIE_HARD_DEBUG, rd32(rc + PCIE_MISC_HARD_PCIE_HARD_DEBUG) & ~(1u << 27));
    udelay(100);
    /* SCB access, CFG read UR mode, 128-byte bursts, RCB modes */
    v = rd32(rc + PCIE_MISC_MISC_CTRL);
    v &= ~0x300000u;
    v |= (1u << 12) | (1u << 13) | (1u << 10) | (1u << 7);
    wr32(rc + PCIE_MISC_MISC_CTRL, v);
    /* inbound: PCI address == CPU address, 2 GB (RC_BAR2), SCB0 size to match */
    wr32(rc + PCIE_MISC_RC_BAR2_CONFIG_LO, 16u);     /* offset 0, size code 31-15 */
    wr32(rc + PCIE_MISC_RC_BAR2_CONFIG_HI, 0);
    v = rd32(rc + PCIE_MISC_MISC_CTRL);
    v = (v & ~0xf8000000u) | (16u << 27);
    wr32(rc + PCIE_MISC_MISC_CTRL, v);
    wr32(rc + PCIE_MISC_RC_BAR1_CONFIG_LO, rd32(rc + PCIE_MISC_RC_BAR1_CONFIG_LO) & ~0x1fu);
    wr32(rc + PCIE_MISC_RC_BAR3_CONFIG_LO, rd32(rc + PCIE_MISC_RC_BAR3_CONFIG_LO) & ~0x1fu);
    wr32(rc + PCIE_MSI_INTR2_MASK_SET, 0xffffffffu);
    wr32(rc + PCIE_MSI_INTR2_CLR, 0xffffffffu);
    /* PERST# off, 100 ms per the CEM spec, then up to 100 ms more for the link */
    wr32(rc + PCIE_RGR1_SW_INIT_1, rd32(rc + PCIE_RGR1_SW_INIT_1) & ~1u);
    mdelay(100);
    for (int i = 0; i < 20 && !link_up(); i++) mdelay(5);
    if (!link_up()) {
        xlog("[FRF-XHCI] FAIL: PCIe link down (status %08x)\n", rd32(rc + PCIE_MISC_PCIE_STATUS));
        return -1;
    }
    if (!(rd32(rc + PCIE_MISC_PCIE_STATUS) & (1u << 7))) {
        xlog("[FRF-XHCI] FAIL: PCIe controller is in endpoint mode\n");
        return -1;
    }
    /* outbound window 0: CPU mmio_cpu -> PCI mmio_pci */
    wr32(rc + PCIE_MEM_WIN0_LO, mmio_pci);
    wr32(rc + PCIE_MEM_WIN0_HI, 0);
    uint32_t base_mb = (uint32_t)(mmio_cpu >> 20), limit_mb = (uint32_t)((mmio_cpu + mmio_size - 1) >> 20);
    v = rd32(rc + PCIE_MEM_WIN0_BASE_LIMIT);
    v = (v & ~0xfff0u) | ((base_mb << 4) & 0xfff0u);
    v = (v & ~0xfff00000u) | ((limit_mb << 20) & 0xfff00000u);
    wr32(rc + PCIE_MEM_WIN0_BASE_LIMIT, v);
    wr32(rc + PCIE_MEM_WIN0_BASE_HI,  (rd32(rc + PCIE_MEM_WIN0_BASE_HI)  & ~0xffu) | ((base_mb  >> 12) & 0xffu));
    wr32(rc + PCIE_MEM_WIN0_LIMIT_HI, (rd32(rc + PCIE_MEM_WIN0_LIMIT_HI) & ~0xffu) | ((limit_mb >> 12) & 0xffu));
    /* show the RC as a PCI-PCI bridge, BAR2 little-endian, no ASPM */
    wr32(rc + PCIE_RC_CFG_PRIV1_ID_VAL3, (rd32(rc + PCIE_RC_CFG_PRIV1_ID_VAL3) & ~0xffffffu) | 0x060400u);
    wr32(rc + PCIE_RC_CFG_VENDOR_SPECIFIC_REG1, rd32(rc + PCIE_RC_CFG_VENDOR_SPECIFIC_REG1) & ~0xcu);
    wr32(rc + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY, rd32(rc + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY) & ~0xc00u);
    xlog("[FRF-XHCI] PCIe link up (RC rev %04x, lnksta %04x)\n",
            rd32(rc + PCIE_MISC_REVISION) & 0xffffu, rd16(rc + 0x00ac + 0x12));
    return 0;
}

/* bus 0 = the RC bridge, bus 1 device 0 = the VL805 */
static int pcie_enumerate(void)
{
    cfg_wr(0, 0x18, (1u << 16) | (1u << 8) | 0u);                 /* primary 0, secondary 1, subordinate 1 */
    uint32_t mb = (mmio_pci >> 16) & 0xfff0u;
    cfg_wr(0, 0x20, (mb << 16) | mb);                              /* memory window: first MB of the outbound */
    cfg_wr(0, 0x24, 0x0000fff0u);                                  /* no prefetchable window */
    cfg_wr(0, 0x04, cfg_rd(0, 0x04) | 0x6u);                       /* memory + bus master */
    uint32_t id = cfg_rd(1, 0x00);
    xlog("[FRF-XHCI] bus 1 device 0: %04x:%04x class %06x\n", id & 0xffff, id >> 16, cfg_rd(1, 0x08) >> 8);
    if ((id & 0xffff) == 0xffff || id == 0) { xlog("[FRF-XHCI] FAIL: no device behind the PCIe bridge\n"); return -1; }
    uint32_t bar = cfg_rd(1, 0x10);
    cfg_wr(1, 0x10, mmio_pci);
    if ((bar & 6u) == 4u) cfg_wr(1, 0x14, 0);                      /* 64-bit BAR: upper half 0 */
    cfg_wr(1, 0x04, (cfg_rd(1, 0x04) & ~0x400u) | 0x6u);           /* memory + bus master, INTx on (unused) */
    xlog("[FRF-XHCI] VL805 BAR0 = %08x (was %08x), fw %08x\n", cfg_rd(1, 0x10), bar, cfg_rd(1, 0x50));
    return 0;
}

/* ---- VideoCore mailbox: reload the VL805's firmware after the PCIe reset --- */
#define MBOX_READ   0xf200b880u
#define MBOX_STATUS 0xf200b898u
#define MBOX_WRITE  0xf200b8a0u
static int vl805_reload(volatile uint32_t *req)
{
    req[0] = LE32(7 * 4); req[1] = 0; req[2] = LE32(0x00030058u);
    req[3] = LE32(4); req[4] = LE32(4); req[5] = LE32(0x00100000u); req[6] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t addr = (uint32_t)(uintptr_t)req;
    uint64_t t = now_ticks(), hz = tick_hz();
    while (rd32(MBOX_STATUS) & (1u << 31)) if (now_ticks() - t > hz) return -2;
    wr32(MBOX_WRITE, addr | 8u);
    t = now_ticks();
    for (;;) {
        if (!(rd32(MBOX_STATUS) & (1u << 30))) {
            uint32_t r = rd32(MBOX_READ);
            if ((r & 15u) == 8u && (r & ~15u) == addr) break;
        }
        if (now_ticks() - t > 2 * hz) return -3;
    }
    return (LE32(req[1]) == 0x80000000u) ? 0 : -4;
}

/* ==== xHCI ====================================================================== */
#define NTRB_CMD   64
#define NTRB_EVT   256
#define NTRB_XFER  64
#define MAX_SLOTS  32
#define MAX_HID    3

/* pool layout (offsets in the 2 MB pool) */
#define P_DCBAA    0x000000u    /* 256 * 8 */
#define P_ERST     0x000800u
#define P_MBOX     0x000900u
#define P_SPARR    0x001000u    /* scratchpad pointer array */
#define P_CMD      0x002000u
#define P_EVT      0x003000u    /* 256 TRBs = 4 KB */
#define P_SPPAGES  0x010000u    /* scratchpad pages, up to 0x40000 */
#define P_SLOTS    0x040000u    /* 16 KB per slot */

struct ring { volatile uint32_t *t; uint32_t phys; int n, enq, cyc; };

#include "alice_hid.c"            /* a mouse's report layout, for its wheel */

struct hid_ep {
    int dci, kind, mps, ival;    /* kind: 1 keyboard, 2 mouse */
    int rp;                      /* mouse in report protocol: hm says where its fields are */
    struct hid_mouse hm;
    struct ring r;
    volatile uint8_t *buf;
    int dead;
};

struct dev {
    int used, port, speed, mps0, nhid;
    int pt;                                /* handed to Neptune (frf_xhci_pt.c) */
    int numcfg;                            /* bNumConfigurations */
    uint32_t route;
    int tt_slot, tt_port;
    int is_hub, nports;
    int child[16];                         /* hub: slot on each port */
    uint8_t failed[16];                    /* hub: port gave up until unplugged */
    volatile uint32_t *in, *out;          /* input / output device contexts */
    struct ring ep0;
    volatile uint8_t *ctl;                 /* 512-byte control buffer */
    struct hid_ep hid[MAX_HID];
};

static uintptr_t xb, op, rt, db;    /* capability, operational, runtime, doorbell bases */
static uintptr_t pool;
static int ctx_sz, max_slots, max_ports;
static uint8_t port_usb3[256];       /* from the supported-protocol capabilities */
static int port_slot[256];
static struct dev devs[MAX_SLOTS + 1];
static struct ring cmd;
static volatile uint32_t *evt;
static int evt_deq, evt_ccs = 1;
static unsigned n_kbd, n_mouse;

static void ring_init(struct ring *r, uintptr_t mem, int n)
{
    r->t = (volatile uint32_t *)mem; r->phys = (uint32_t)mem; r->n = n; r->enq = 0; r->cyc = 1;
    for (int i = 0; i < n * 4; i++) r->t[i] = 0;
    volatile uint32_t *l = r->t + (n - 1) * 4;     /* link back to the start, toggle cycle */
    l[0] = LE32(r->phys); l[1] = 0; l[2] = 0; l[3] = LE32((6u << 10) | 2u);
}
static uint32_t ring_put(struct ring *r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl)
{
    volatile uint32_t *t = r->t + r->enq * 4;
    uint32_t at = r->phys + r->enq * 16;
    t[0] = LE32(p0); t[1] = LE32(p1); t[2] = LE32(st);
    __asm__ volatile("dmb oshst" ::: "memory");
    t[3] = LE32(ctl | (uint32_t)r->cyc);
    if (++r->enq == r->n - 1) {
        volatile uint32_t *l = r->t + (r->n - 1) * 4;
        __asm__ volatile("dmb oshst" ::: "memory");
        l[3] = LE32((6u << 10) | 2u | (uint32_t)r->cyc);
        r->cyc ^= 1; r->enq = 0;
    }
    __asm__ volatile("dsb oshst" ::: "memory");
    return at;
}
static inline void ring_db(int slot, int target) { wr32(db + 4u * (uint32_t)slot, (uint32_t)target); }

/* ---- events -------------------------------------------------------------------- */
struct evt { uint32_t p0, p1, st, ctl; };
static int evt_get(struct evt *e)
{
    volatile uint32_t *t = evt + evt_deq * 4;
    uint32_t c = LE32(t[3]);
    if ((c & 1u) != (uint32_t)evt_ccs) return 0;
    __asm__ volatile("dmb oshld" ::: "memory");
    e->p0 = LE32(t[0]); e->p1 = LE32(t[1]); e->st = LE32(t[2]); e->ctl = c;
    if (++evt_deq == NTRB_EVT) { evt_deq = 0; evt_ccs ^= 1; }
    wr64(rt + 0x38, (uint64_t)((uint32_t)(uintptr_t)evt + evt_deq * 16) | 8u);   /* ERDP, EHB cleared */
    return 1;
}

static void on_transfer(const struct evt *e);
static volatile uint8_t port_pending[256];

/* Wait for a command completion / a transfer on (slot, dci); everything else
   met on the way is handled as it comes. */
static int wait_event(int want_type, uint32_t want_ptr, int slot, int dci, struct evt *out, uint32_t ms)
{
    uint64_t end = now_ticks() + tick_hz() * ms / 1000u;
    struct evt e;
    while ((int64_t)(now_ticks() - end) < 0) {
        if (!evt_get(&e)) { __asm__ volatile("yield"); continue; }
        int type = (int)((e.ctl >> 10) & 63u);
        if (type == want_type) {
            if (type == 33 && e.p0 == want_ptr) { *out = e; return (int)(e.st >> 24); }
            if (type == 32 && (int)(e.ctl >> 24) == slot && (int)((e.ctl >> 16) & 31u) == dci) { *out = e; return (int)(e.st >> 24); }
        }
        if (type == 32) on_transfer(&e);
        else if (type == 34 && (e.p0 >> 24) < 256) port_pending[e.p0 >> 24] = 1;   /* the main loop's */
    }
    return -1;
}

static int command(uint32_t p0, uint32_t p1, uint32_t ctl, int *slot_out)
{
    uint32_t at = ring_put(&cmd, p0, p1, 0, ctl);
    ring_db(0, 0);
    struct evt e = { 0, 0, 0, 0 };
    int cc = wait_event(33, at, 0, 0, &e, 2000);
    if (slot_out) *slot_out = (int)(e.ctl >> 24);
    return cc;
}

/* ---- contexts ------------------------------------------------------------------ */
static inline volatile uint32_t *ictx(struct dev *d, int idx) { return d->in + idx * (ctx_sz / 4); }   /* 0 control, 1 slot, 2.. EPs */
static void ctx_clear(volatile uint32_t *c, int bytes) { for (int i = 0; i < bytes / 4; i++) c[i] = 0; }

static int control(int slot, uint8_t rt_, uint8_t rq, uint16_t val, uint16_t idx, uint16_t len)
{
    struct dev *d = &devs[slot];
    int in = (rt_ & 0x80) != 0;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_put(&d->ep0, (uint32_t)rt_ | ((uint32_t)rq << 8) | ((uint32_t)val << 16), (uint32_t)idx | ((uint32_t)len << 16),
             8, (2u << 10) | (1u << 6) | (trt << 16));
    if (len) ring_put(&d->ep0, (uint32_t)(uintptr_t)d->ctl, 0, len, (3u << 10) | (in ? 1u << 16 : 0));
    ring_put(&d->ep0, 0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0 : 1u << 16));
    ring_db(slot, 1);
    struct evt e = { 0, 0, 0, 0 };
    int cc = wait_event(32, 0, slot, 1, &e, 1000);
    return (cc == 1 || cc == 13) ? (int)(len - (e.st & 0xffffffu)) : -(cc < 0 ? 999 : cc);
}

static int ep_interval(int speed, int binterval)
{
    if (speed == 1 || speed == 2) {           /* FS/LS: frames -> 2^n * 125 us */
        int n = 3; while (n < 10 && (1 << (n + 1)) <= binterval * 8) n++;
        return n;
    }
    if (binterval < 1) binterval = 1;
    if (binterval > 16) binterval = 16;
    return binterval - 1;
}

#include "frf_xhci_pt.c"

/* ---- devices: root ports and the hub behind them --------------------------
   On a Pi 4 every USB 2 device - keyboards and mice included - sits behind
   the VL805's built-in hub (2109:3431) on root port 1; only SuperSpeed
   devices reach the root ports directly. So a hub is followed one level
   deep: its ports are powered, polled every 50 ms with GET_STATUS, reset
   when something connects, and the device is addressed through the hub's
   transaction translator. Hubs behind hubs are not followed. */

static void forget_slot(int s);
static void hub_forget_children(struct dev *h)
{
    for (int p = 1; p <= h->nports && p < 16; p++)
        if (h->child[p]) { int c = h->child[p]; h->child[p] = 0; forget_slot(c); }
}
static void forget_slot(int s)
{
    if (s <= 0 || s > MAX_SLOTS || !devs[s].used) return;
    struct dev *d = &devs[s];
    if (d->is_hub) hub_forget_children(d);
    int had_kbd = 0;
    for (int i = 0; i < d->nhid; i++) {
        if (d->hid[i].kind == 1) { had_kbd = 1; if (n_kbd) n_kbd--; }
        if (d->hid[i].kind == 2 && n_mouse) n_mouse--;
    }
    if (had_kbd) frf_native_usb_keyboard_force_release();
    if (d->pt) pt_detach(s);
    d->used = 0; d->nhid = 0; d->is_hub = 0;
    (void)command(0, 0, (10u << 10) | ((uint32_t)s << 24), 0);     /* Disable Slot */
    xlog("[FRF-XHCI] slot %d: device gone\n", s);
    frf_post_usb_devices(n_kbd, n_mouse, 0, 0);
}

static int port_reset(int port, uint32_t *psc)
{
    uintptr_t pr = op + 0x400u + 0x10u * (uint32_t)(port - 1);
    uint32_t v = rd32(pr);
    const uint32_t keep = (1u << 9) | (3u << 14) | (7u << 25);   /* PP, PIC, wake bits */
    if (!(v & 1u)) return -1;
    if (!port_usb3[port] || !(v & 2u)) {
        wr32(pr, (v & keep) | (1u << 4));                        /* PR */
        for (int i = 0; i < 100 && !(rd32(pr) & (1u << 21)); i++) mdelay(1);
        mdelay(10);
    } else {
        mdelay(20);                                              /* SuperSpeed trains by itself */
    }
    v = rd32(pr);
    wr32(pr, (v & keep) | (v & (0x7fu << 17)));                  /* clear the change bits */
    *psc = rd32(pr);
    return (*psc & 2u) ? 0 : -2;
}

static void hub_setup(int slot);

/* Address and configure one device. root: its root port; route: the hub port
   path; tt_slot/tt_port: the high-speed hub whose TT a LS/FS device uses. */
/* ---- virtual eject: flip "driver CD" devices into their real mode ------------ */
/* Some USB network dongles (Realtek RTL8152/8153/8156: 0bda:8151) first show up
   as a CD-ROM holding the Windows driver, and only become a NIC once sent a
   vendor SCSI command - what usb_modeswitch does on Linux. The kernel does it
   here, so the adapter reappears with its network ID and goes to Neptune.
   "frf_usb.nomodeswitch" on cmdline.txt turns this off. */
static int bootarg_has(const char *w)
{
    of_node_t *ch = dt_find_node("/chosen");
    of_property_t *ba = ch ? dt_find_property(ch, "bootargs") : 0;
    if (!ba || !ba->op_value) return 0;
    for (const char *s = ba->op_value; *s; s++) {
        const char *a = s, *b = w;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

struct modeswitch { uint16_t vid, pid; const char *what; uint8_t datalen, dir_in, cdb[6]; };
static const struct modeswitch ms_table[] = {
    { 0x0bda, 0x8151, "Realtek RTL815x network adapter", 0xc0, 1, { 0xe0, 0, 0, 0, 0, 0 } },   /* usb_modeswitch 0bda:8151 */
};
static uint32_t ms_where[8];
static uint64_t ms_when[8];
static int ms_next;

static int bulk_xfer(int slot, int dci, struct ring *r, volatile uint8_t *buf, uint32_t len, uint32_t ms)
{
    ring_put(r, (uint32_t)(uintptr_t)buf, 0, len, (1u << 10) | (1u << 5) | (1u << 2));   /* Normal, IOC, ISP */
    ring_db(slot, dci);
    struct evt e = { 0, 0, 0, 0 };
    int cc = wait_event(32, 0, slot, dci, &e, ms);
    return (cc == 1 || cc == 13) ? (int)(len - (e.st & 0xffffffu)) : -(cc < 0 ? 999 : cc);
}

/* 1: the command went out, the device will drop off and come back; 0: not ours */
static int modeswitch(int slot, uint16_t vid, uint16_t pid, const uint8_t *desc, int n, uint8_t cfgval, const char *where)
{
    const struct modeswitch *m = 0;
    for (unsigned i = 0; i < sizeof ms_table / sizeof ms_table[0]; i++)
        if (ms_table[i].vid == vid && ms_table[i].pid == pid) m = &ms_table[i];
    if (!m) return 0;
    struct dev *d = &devs[slot];
    if (bootarg_has("frf_usb.nomodeswitch")) { xlog("[FRF-XHCI] %s: %s in driver-CD mode, mode switch off (cmdline)\n", where, m->what); return 0; }
    uint32_t key = ((uint32_t)d->port << 20) | (d->route & 0xfffffu);
    uint64_t t = now_ticks();
    for (int i = 0; i < 8; i++)
        if (ms_when[i] && ms_where[i] == key && t - ms_when[i] < tick_hz() * 15u) {
            xlog("[FRF-XHCI] %s: %s still in driver-CD mode after a switch - left to Neptune\n", where, m->what);
            return 0;
        }

    /* the mass-storage interface's bulk endpoints */
    int in_ep = 0, out_ep = 0, in_mps = 0, out_mps = 0, msc = 0;
    for (int i = 0; i + 2 <= n && desc[i] >= 2; i += desc[i]) {
        if (desc[i + 1] == 4 && i + 9 <= n) msc = desc[i + 5] == 8;
        else if (desc[i + 1] == 5 && i + 7 <= n && msc && (desc[i + 3] & 3) == 2) {
            int mps = (desc[i + 4] | (desc[i + 5] << 8)) & 0x7ff;
            if ((desc[i + 2] & 0x80) && !in_ep) { in_ep = desc[i + 2] & 15; in_mps = mps; }
            if (!(desc[i + 2] & 0x80) && !out_ep) { out_ep = desc[i + 2] & 15; out_mps = mps; }
        }
    }
    if (!in_ep || !out_ep) { xlog("[FRF-XHCI] %s: %s: no bulk endpoints, not switched\n", where, m->what); return 0; }
    if (control(slot, 0x00, 9, cfgval, 0, 0) < 0) { xlog("[FRF-XHCI] %s: mode switch: SET_CONFIGURATION failed\n", where); return 0; }

    uintptr_t area = (uintptr_t)d->out;
    struct ring rin, rout;
    ring_init(&rout, area + 0x2400, NTRB_XFER);
    ring_init(&rin,  area + 0x2800, NTRB_XFER);
    int dci_out = 2 * out_ep, dci_in = 2 * in_ep + 1, max_dci = dci_in > dci_out ? dci_in : dci_out;
    ctx_clear(d->in, 0x800);
    ictx(d, 0)[1] = LE32(1u | (1u << dci_out) | (1u << dci_in));
    volatile uint32_t *ep = ictx(d, dci_out + 1);
    ep[1] = LE32((3u << 1) | (2u << 3) | ((uint32_t)out_mps << 16));     /* bulk OUT */
    ep[2] = LE32(rout.phys | 1u);
    ep[4] = LE32((uint32_t)out_mps);
    ep = ictx(d, dci_in + 1);
    ep[1] = LE32((3u << 1) | (6u << 3) | ((uint32_t)in_mps << 16));      /* bulk IN */
    ep[2] = LE32(rin.phys | 1u);
    ep[4] = LE32((uint32_t)in_mps);
    ictx(d, 1)[0] = LE32(((uint32_t)max_dci << 27) | ((uint32_t)d->speed << 20) | (d->route & 0xfffffu));
    ictx(d, 1)[1] = LE32((uint32_t)d->port << 16);
    if (d->tt_slot) ictx(d, 1)[2] = LE32((uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8));
    int cc = command((uint32_t)(uintptr_t)d->in, 0, (12u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != 1) { xlog("[FRF-XHCI] %s: mode switch: Configure Endpoint failed (cc %d)\n", where, cc); return 0; }

    volatile uint8_t *cbw = (volatile uint8_t *)(area + 0x3000), *data = cbw + 0x100, *csw = cbw + 0x200;
    static const uint8_t sig[8] = { 'U', 'S', 'B', 'C', 0x08, 0x60, 0xd9, 0xa9 };
    for (int i = 0; i < 31; i++) cbw[i] = 0;
    for (int i = 0; i < 8; i++) cbw[i] = sig[i];
    cbw[8] = m->datalen;                          /* dCBWDataTransferLength, little-endian */
    cbw[12] = m->dir_in ? 0x80 : 0x00;
    cbw[14] = 6;
    for (int i = 0; i < 6; i++) cbw[15 + i] = m->cdb[i];
    xlog("[FRF-XHCI] %s: %s in driver-CD mode - virtual eject (mode switch)\n", where, m->what);
    ms_where[ms_next] = key; ms_when[ms_next] = t; ms_next = (ms_next + 1) & 7;

    int r = bulk_xfer(slot, dci_out, &rout, cbw, 31, 1000);
    if (r != 31) { xlog("[FRF-XHCI] %s: mode switch: command not accepted (%d)\n", where, r); return 1; }
    if (m->datalen) {
        r = bulk_xfer(slot, m->dir_in ? dci_in : dci_out, m->dir_in ? &rin : &rout, data, m->datalen, 1000);
        if (r < 0) { xlog("[FRF-XHCI] %s: mode switch sent (no data phase, %d) - waiting for it to reappear\n", where, r); return 1; }
    }
    r = bulk_xfer(slot, dci_in, &rin, csw, 13, 1000);
    xlog("[FRF-XHCI] %s: mode switch sent (status %d) - waiting for it to reappear as a network adapter\n",
         where, r == 13 ? csw[12] : -1);
    return 1;
}

static int enumerate(int root, uint32_t route, int speed, int tt_slot, int tt_port, const char *where)
{
    int slot = 0;
    int cc = command(0, 0, 9u << 10, &slot);                      /* Enable Slot */
    if (cc != 1 || slot <= 0 || slot > MAX_SLOTS) { xlog("[FRF-XHCI] %s: Enable Slot failed (cc %d slot %d)\n", where, cc, slot); return 0; }
    struct dev *d = &devs[slot];
    uintptr_t area = pool + P_SLOTS + (uint32_t)slot * 0x4000u;
    for (int i = 0; i < 0x4000 / 4; i++) ((volatile uint32_t *)area)[i] = 0;
    for (unsigned i = 0; i < sizeof *d; i++) ((uint8_t *)d)[i] = 0;
    d->used = 1; d->port = root; d->speed = speed; d->route = route;
    d->tt_slot = tt_slot; d->tt_port = tt_port;
    d->out = (volatile uint32_t *)area;
    d->in  = (volatile uint32_t *)(area + 0x800);
    ring_init(&d->ep0, area + 0x1400, NTRB_XFER);       /* after the input context (up to 33 x 64 bytes) */
    d->ctl = (volatile uint8_t *)(area + 0x2000);
    d->mps0 = speed == 4 ? 512 : speed == 2 ? 8 : 64;
    ((volatile uint64_t *)(pool + P_DCBAA))[slot] = __builtin_bswap64((uint64_t)(uintptr_t)d->out);

    /* Address Device: slot context + EP0 */
    ctx_clear(d->in, 0x800);
    ictx(d, 0)[1] = LE32(3u);                                     /* add slot + EP0 */
    ictx(d, 1)[0] = LE32((1u << 27) | ((uint32_t)speed << 20) | (route & 0xfffffu));
    ictx(d, 1)[1] = LE32((uint32_t)root << 16);
    if (tt_slot) ictx(d, 1)[2] = LE32((uint32_t)tt_slot | ((uint32_t)tt_port << 8));
    volatile uint32_t *e0 = ictx(d, 2);
    e0[1] = LE32((3u << 1) | (4u << 3) | ((uint32_t)d->mps0 << 16));   /* CErr 3, control */
    e0[2] = LE32(d->ep0.phys | 1u);
    e0[4] = LE32(8u);
    cc = command((uint32_t)(uintptr_t)d->in, 0, (11u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != 1) { xlog("[FRF-XHCI] %s: Address Device failed (cc %d)\n", where, cc); forget_slot(slot); return 0; }
    mdelay(2);

    /* device descriptor: 8 bytes first, fix EP0's packet size if needed */
    int n = control(slot, 0x80, 6, 0x0100, 0, 8);
    if (n < 8) { xlog("[FRF-XHCI] %s: no device descriptor (%d)\n", where, n); forget_slot(slot); return 0; }
    int mps = speed == 4 ? (1 << d->ctl[7]) : d->ctl[7];
    if (mps && mps != d->mps0) {
        d->mps0 = mps;
        ctx_clear(d->in, 0x800);
        ictx(d, 0)[1] = LE32(2u);                                 /* evaluate EP0 */
        ictx(d, 2)[1] = LE32((3u << 1) | (4u << 3) | ((uint32_t)mps << 16));
        (void)command((uint32_t)(uintptr_t)d->in, 0, (13u << 10) | ((uint32_t)slot << 24), 0);
    }
    n = control(slot, 0x80, 6, 0x0100, 0, 18);
    uint16_t vid = (uint16_t)(d->ctl[8] | (d->ctl[9] << 8)), pid = (uint16_t)(d->ctl[10] | (d->ctl[11] << 8));
    uint8_t dclass = d->ctl[4];
    d->numcfg = n >= 18 && d->ctl[17] ? d->ctl[17] : 1;        /* before the config descriptor reuses ctl */
    static const char *const sp[] = { "?", "full", "low", "high", "super", "super+" };
    xlog("[FRF-XHCI] %s: %04x:%04x, %s speed, class %02x, slot %d\n", where, vid, pid, sp[speed < 6 ? speed : 0], dclass, slot);

    /* configuration descriptor */
    n = control(slot, 0x80, 6, 0x0200, 0, 9);
    if (n < 9) { xlog("[FRF-XHCI] %s: no configuration descriptor\n", where); return slot; }
    int total = d->ctl[2] | (d->ctl[3] << 8);
    if (total > 512) total = 512;
    n = control(slot, 0x80, 6, 0x0200, 0, (uint16_t)total);
    if (n < 9) return slot;
    uint8_t cfgval = d->ctl[5];
    uint8_t desc[512];
    for (int i = 0; i < n; i++) desc[i] = d->ctl[i];

    if (dclass == 9) {                                            /* a hub */
        if (route) { xlog("[FRF-XHCI] %s: a hub behind a hub - not followed\n", where); return slot; }
        if (control(slot, 0x00, 9, cfgval, 0, 0) < 0) { xlog("[FRF-XHCI] %s: hub SET_CONFIGURATION failed\n", where); return slot; }
        hub_setup(slot);
        return slot;
    }
    if (modeswitch(slot, vid, pid, desc, n, cfgval, where)) return slot;   /* it will reconnect */

    int cur_if = -1, cur_kind = 0, max_dci = 1, cur_rdlen = 0;
    int ifnum[MAX_HID], rdlen[MAX_HID];
    for (int i = 0; i + 2 <= n && desc[i] >= 2; i += desc[i]) {
        if (desc[i + 1] == 4 && i + 9 <= n) {                    /* interface */
            cur_if = desc[i + 2];
            cur_kind = (desc[i + 5] == 3 && desc[i + 6] == 1) ? (desc[i + 7] == 1 ? 1 : desc[i + 7] == 2 ? 2 : 0) : 0;
            cur_rdlen = 0;
            if (desc[i + 5] != 3 || desc[i + 6] != 1)
                xlog("[FRF-XHCI] %s: interface %d class %02x/%02x/%02x - left alone\n", where, cur_if, desc[i + 5], desc[i + 6], desc[i + 7]);
        } else if (desc[i + 1] == 0x21 && i + 9 <= n && desc[i + 6] == 0x22) {          /* HID: report descriptor length */
            cur_rdlen = desc[i + 7] | (desc[i + 8] << 8);
        } else if (desc[i + 1] == 5 && i + 7 <= n && cur_kind && d->nhid < MAX_HID) {   /* endpoint */
            uint8_t ea = desc[i + 2], at = desc[i + 3];
            if ((ea & 0x80) && (at & 3) == 3) {
                struct hid_ep *h = &d->hid[d->nhid];
                h->kind = cur_kind;
                h->dci = 2 * (ea & 15) + 1;
                h->mps = (desc[i + 4] | (desc[i + 5] << 8)) & 0x7ff;
                if (h->mps > 64) h->mps = 64;
                h->dead = 0;
                h->rp = 0;
                rdlen[d->nhid] = cur_rdlen;
                ring_init(&h->r, area + 0x2400 + (uint32_t)d->nhid * 0x400, NTRB_XFER);
                h->buf = (volatile uint8_t *)(area + 0x3000 + (uint32_t)d->nhid * 0x100);
                h->ival = ep_interval(speed, desc[i + 6]);
                ifnum[d->nhid] = cur_if;
                if (h->dci > max_dci) max_dci = h->dci;
                d->nhid++;
                cur_kind = 0;
            }
        }
    }
    if (!d->nhid) {
        pt_attach(slot, vid, pid, where);           /* not a keyboard or mouse: Neptune's */
        return slot;
    }
    if (control(slot, 0x00, 9, cfgval, 0, 0) < 0) { xlog("[FRF-XHCI] %s: SET_CONFIGURATION failed\n", where); return slot; }
    for (int k = 0; k < d->nhid; k++) {
        struct hid_ep *h = &d->hid[k];
        /* a mouse with a wheel: report protocol, laid out as its report descriptor says. Any
           other mouse, or one whose descriptor cannot be read or understood, and every
           keyboard: boot protocol (buttons, X, Y) as before */
        if (h->kind == 2 && rdlen[k] > 0 && rdlen[k] <= 512) {
            int got = control(slot, 0x81, 6, 0x2200, (uint16_t)ifnum[k], (uint16_t)rdlen[k]);
            if (got > 0) {
                uint8_t rd[512];
                for (int i = 0; i < got; i++) rd[i] = d->ctl[i];
                if (hid_mouse_parse(rd, got, &h->hm) && (h->hm.wheel.size || h->hm.pan.size)
                    && control(slot, 0x21, 0x0b, 1, (uint16_t)ifnum[k], 0) >= 0)          /* SET_PROTOCOL report */
                    h->rp = 1;
            }
        }
        if (!h->rp)
            (void)control(slot, 0x21, 0x0b, 0, (uint16_t)ifnum[k], 0);     /* SET_PROTOCOL boot */
        (void)control(slot, 0x21, 0x0a, 0, (uint16_t)ifnum[k], 0);     /* SET_IDLE 0 (stall is fine) */
    }

    /* Configure Endpoint: the interrupt IN endpoints */
    ctx_clear(d->in, 0x800);
    uint32_t add = 1u;
    for (int k = 0; k < d->nhid; k++) {
        struct hid_ep *h = &d->hid[k];
        add |= 1u << h->dci;
        volatile uint32_t *ep = ictx(d, h->dci + 1);
        ep[0] = LE32((uint32_t)h->ival << 16);
        ep[1] = LE32((3u << 1) | (7u << 3) | ((uint32_t)h->mps << 16));   /* CErr 3, interrupt IN */
        ep[2] = LE32(h->r.phys | 1u);
        ep[4] = LE32(((uint32_t)h->mps << 16) | (uint32_t)h->mps);       /* max ESIT payload, average TRB length */
    }
    ictx(d, 0)[1] = LE32(add);
    ictx(d, 1)[0] = LE32(((uint32_t)max_dci << 27) | ((uint32_t)speed << 20) | (route & 0xfffffu));
    ictx(d, 1)[1] = LE32((uint32_t)root << 16);
    if (tt_slot) ictx(d, 1)[2] = LE32((uint32_t)tt_slot | ((uint32_t)tt_port << 8));
    cc = command((uint32_t)(uintptr_t)d->in, 0, (12u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != 1) { xlog("[FRF-XHCI] %s: Configure Endpoint failed (cc %d)\n", where, cc); d->nhid = 0; return slot; }

    for (int k = 0; k < d->nhid; k++) {
        struct hid_ep *h = &d->hid[k];
        ring_put(&h->r, (uint32_t)(uintptr_t)h->buf, 0, (uint32_t)h->mps, (1u << 10) | (1u << 5) | (1u << 2));   /* Normal, IOC, ISP */
        ring_db(slot, h->dci);
        if (h->kind == 1) n_kbd++; else n_mouse++;
        xlog("[FRF-XHCI] %s: %s ready (endpoint %d, %d bytes) -> Amiga %s%s\n", where,
                h->kind == 1 ? "keyboard" : "mouse", (h->dci - 1) / 2, h->mps, h->kind == 1 ? "keyboard" : "mouse port",
                h->rp ? (h->hm.pan.size ? ", wheels for Alice68-Wheel" : ", wheel for Alice68-Wheel") : "");
    }
    frf_post_usb_devices(n_kbd, n_mouse, 0, 0);
    return slot;
}

/* ---- the hub ---------------------------------------------------------------- */
static void hub_setup(int slot)
{
    struct dev *h = &devs[slot];
    int n = control(slot, 0xa0, 6, 0x2900, 0, 9);                 /* hub descriptor */
    if (n < 7) { xlog("[FRF-XHCI] hub slot %d: no hub descriptor\n", slot); return; }
    h->nports = h->ctl[2] > 15 ? 15 : h->ctl[2];
    uint16_t chr = (uint16_t)(h->ctl[3] | (h->ctl[4] << 8));
    int pgood = h->ctl[5] * 2;
    int ttt = (chr >> 5) & 3;
    /* tell the xHC this slot is a hub: Configure Endpoint with only the slot context */
    ctx_clear(h->in, 0x800);
    ictx(h, 0)[1] = LE32(1u);
    ictx(h, 1)[0] = LE32((1u << 27) | (1u << 26) | ((uint32_t)h->speed << 20) | (h->route & 0xfffffu));
    ictx(h, 1)[1] = LE32(((uint32_t)h->nports << 24) | ((uint32_t)h->port << 16));
    ictx(h, 1)[2] = LE32((uint32_t)ttt << 16);
    /* as Linux's xhci_update_hub_device on an xHCI 1.0: Evaluate Context,
       Configure Endpoint if that is refused */
    int cc = command((uint32_t)(uintptr_t)h->in, 0, (13u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != 1) cc = command((uint32_t)(uintptr_t)h->in, 0, (12u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != 1) xlog("[FRF-XHCI] hub slot %d: hub context not taken (cc %d)\n", slot, cc);
    for (int p = 1; p <= h->nports; p++)
        (void)control(slot, 0x23, 3, 8, (uint16_t)p, 0);         /* SET_FEATURE PORT_POWER */
    mdelay(pgood > 100 ? pgood : 100);
    h->is_hub = 1;
    xlog("[FRF-XHCI] hub slot %d: %d ports powered - following them\n", slot, h->nports);
}

static void hub_poll(int slot)
{
    struct dev *h = &devs[slot];
    for (int p = 1; p <= h->nports; p++) {
        if (control(slot, 0xa3, 0, 0, (uint16_t)p, 4) < 4) continue;   /* GET_STATUS port */
        uint16_t st = (uint16_t)(h->ctl[0] | (h->ctl[1] << 8)), ch = (uint16_t)(h->ctl[2] | (h->ctl[3] << 8));
        if (ch & 1u) (void)control(slot, 0x23, 1, 16, (uint16_t)p, 0);   /* clear C_PORT_CONNECTION */
        if (ch & 2u) (void)control(slot, 0x23, 1, 17, (uint16_t)p, 0);   /* clear C_PORT_ENABLE */
        if (ch & 8u) (void)control(slot, 0x23, 1, 19, (uint16_t)p, 0);   /* clear C_PORT_OVER_CURRENT */
        int connected = st & 1u;
        if (!connected && h->child[p]) {
            int c = h->child[p]; h->child[p] = 0;
            xlog("[FRF-XHCI] hub %d port %d: unplugged\n", slot, p);
            forget_slot(c);
        } else if (connected && !h->child[p] && !h->failed[p]) {
            mdelay(100);                                          /* debounce */
            (void)control(slot, 0x23, 3, 4, (uint16_t)p, 0);     /* SET_FEATURE PORT_RESET */
            int ok = 0;
            for (int i = 0; i < 50; i++) {
                mdelay(5);
                if (control(slot, 0xa3, 0, 0, (uint16_t)p, 4) < 4) continue;
                if (h->ctl[2] & 0x10u) { ok = 1; break; }        /* C_PORT_RESET */
            }
            st = (uint16_t)(h->ctl[0] | (h->ctl[1] << 8));
            (void)control(slot, 0x23, 1, 20, (uint16_t)p, 0);    /* clear C_PORT_RESET */
            mdelay(10);
            if (!ok || !(st & 2u)) { xlog("[FRF-XHCI] hub %d port %d: reset failed (status %04x)\n", slot, p, st); h->failed[p] = 1; continue; }
            int speed = (st & 0x200u) ? 2 : (st & 0x400u) ? 3 : 1;   /* LS / HS / FS */
            char where[24];
            where[0] = 0;
            {   /* "hub N port M" */
                const char *a = "hub "; int k = 0;
                while (*a) where[k++] = *a++;
                where[k++] = (char)('0' + slot / 10 % 10); where[k++] = (char)('0' + slot % 10);
                a = " port "; while (*a) where[k++] = *a++;
                where[k++] = (char)('0' + p % 10); where[k] = 0;
            }
            int tt = (speed != 3 && h->speed == 3);
            int c = enumerate(h->port, (uint32_t)p, speed, tt ? slot : 0, tt ? p : 0, where);
            if (c) h->child[p] = c; else h->failed[p] = 1;
        } else if (!connected) {
            h->failed[p] = 0;
        }
    }
}

/* ---- root ports ------------------------------------------------------------- */
static void root_attach(int port)
{
    uint32_t psc;
    if (port_slot[port]) return;
    if (port_reset(port, &psc) != 0) { xlog("[FRF-XHCI] port %d: reset failed (PORTSC %08x)\n", port, rd32(op + 0x400u + 0x10u * (uint32_t)(port - 1))); return; }
    char where[8] = "port ?";
    where[5] = (char)('0' + port % 10);
    int s = enumerate(port, 0, (int)((psc >> 10) & 15u), 0, 0, where);
    port_slot[port] = s;
}

static void root_change(int port)
{
    uintptr_t pr = op + 0x400u + 0x10u * (uint32_t)(port - 1);
    uint32_t v = rd32(pr);
    const uint32_t keep = (1u << 9) | (3u << 14) | (7u << 25);
    wr32(pr, (v & keep) | (v & (0x7fu << 17)));
    if ((v & 1u) && !port_slot[port]) { mdelay(100); root_attach(port); }
    else if (!(v & 1u) && port_slot[port]) { int s = port_slot[port]; port_slot[port] = 0; xlog("[FRF-XHCI] port %d: unplugged\n", port); forget_slot(s); }
}

/* ---- reports ---------------------------------------------------------------- */
static void on_transfer(const struct evt *e)
{
    int slot = (int)(e->ctl >> 24), dci = (int)((e->ctl >> 16) & 31u), cc = (int)(e->st >> 24);
    if (slot <= 0 || slot > MAX_SLOTS || !devs[slot].used) return;
    if (devs[slot].pt) { pt_on_transfer(e); return; }
    struct dev *d = &devs[slot];
    for (int k = 0; k < d->nhid; k++) {
        struct hid_ep *h = &d->hid[k];
        if (h->dci != dci || h->dead) continue;
        if (cc == 1 || cc == 13) {
            int len = h->mps - (int)(e->st & 0xffffffu);
            uint8_t rep[64];
            for (int i = 0; i < len && i < 64; i++) rep[i] = h->buf[i];
            if (h->kind == 1 && len >= 8) {
                frf_native_usb_keyboard_boot_report(rep);
            } else if (h->kind == 2 && h->rp) {
                struct hid_mouse_report r;
                if (len > 0 && hid_mouse_decode(&h->hm, rep, len < 64 ? len : 64, &r)) {
                    /* the Amiga reads its counters once a frame: keep each step in a byte */
                    int dx = r.dx < -127 ? -127 : r.dx > 127 ? 127 : r.dx;
                    int dy = r.dy < -127 ? -127 : r.dy > 127 ? 127 : r.dy;
                    aga_virtual_mouse_update(dx, dy, r.buttons & 7u);
                    frf_usb_mouse_seen = 1; frf_usb_mouse_buttons = (uint8_t)r.buttons;
                    pt_mouse_extra(r.wheel, r.pan, r.buttons);
                }
            } else if (h->kind == 2 && len >= 3) {
                aga_virtual_mouse_update((int)(int8_t)rep[1], (int)(int8_t)rep[2], rep[0] & 7u);
                frf_usb_mouse_seen = 1; frf_usb_mouse_buttons = rep[0];
            }
            ring_put(&h->r, (uint32_t)(uintptr_t)h->buf, 0, (uint32_t)h->mps, (1u << 10) | (1u << 5) | (1u << 2));
            ring_db(slot, dci);
        } else {
            h->dead = 1;
            xlog("[FRF-XHCI] port %d: %s endpoint stopped (completion %d) - replug it\n", d->port, h->kind == 1 ? "keyboard" : "mouse", cc);
        }
        return;
    }
}

/* ---- controller ------------------------------------------------------------- */
static int xhci_start(void)
{
    xb = mmio_virt;
    uint32_t cap0 = rd32(xb);
    if (cap0 == 0xffffffffu) { xlog("[FRF-XHCI] FAIL: xHCI registers read as all ones\n"); return -1; }
    op = xb + (cap0 & 0xffu);
    uint32_t hcs1 = rd32(xb + 4), hcs2 = rd32(xb + 8), hcc1 = rd32(xb + 0x10);
    db = xb + (rd32(xb + 0x14) & ~3u);
    rt = xb + (rd32(xb + 0x18) & ~31u);
    max_slots = (int)(hcs1 & 0xffu); if (max_slots > MAX_SLOTS) max_slots = MAX_SLOTS;
    max_ports = (int)(hcs1 >> 24);
    ctx_sz = (hcc1 & 4u) ? 64 : 32;
    int nsp = (int)(((hcs2 >> 21) & 0x1fu) << 5 | ((hcs2 >> 27) & 0x1fu));
    xlog("[FRF-XHCI] xHCI %x.%02x: %d slots, %d ports, %d-byte contexts, %d scratchpads\n",
            cap0 >> 24, (cap0 >> 16) & 0xffu, (int)(hcs1 & 0xffu), max_ports, ctx_sz, nsp);

    /* which ports are USB 3 (supported-protocol capabilities) */
    for (uint32_t x = (hcc1 >> 16) << 2; x; ) {
        uint32_t c = rd32(xb + x);
        if ((c & 0xffu) == 2u) {
            uint32_t pc = rd32(xb + x + 8);
            int first = (int)(pc & 0xffu), cnt = (int)((pc >> 8) & 0xffu), major = (int)(c >> 24);
            for (int p = first; p < first + cnt && p < 256; p++) port_usb3[p] = major >= 3;
        }
        uint32_t nx = (c >> 8) & 0xffu;
        x = nx ? x + (nx << 2) : 0;
    }

    /* halt + reset */
    for (int i = 0; i < 200 && (rd32(op + 4) & (1u << 11)); i++) mdelay(1);     /* CNR */
    wr32(op, rd32(op) & ~1u);
    for (int i = 0; i < 100 && !(rd32(op + 4) & 1u); i++) mdelay(1);            /* HCH */
    wr32(op, rd32(op) | 2u);                                                       /* HCRST */
    for (int i = 0; i < 500 && (rd32(op) & 2u); i++) mdelay(1);
    for (int i = 0; i < 500 && (rd32(op + 4) & (1u << 11)); i++) mdelay(1);
    if (rd32(op) & 2u || rd32(op + 4) & (1u << 11)) { xlog("[FRF-XHCI] FAIL: controller reset timed out (USBSTS %08x)\n", rd32(op + 4)); return -1; }

    /* memory */
    for (uint32_t i = 0; i < 0x40000u / 4; i++) ((volatile uint32_t *)pool)[i] = 0;
    if (nsp > 48) { xlog("[FRF-XHCI] FAIL: %d scratchpads is more than the pool holds\n", nsp); return -1; }
    volatile uint64_t *sp = (volatile uint64_t *)(pool + P_SPARR);
    for (int i = 0; i < nsp; i++) sp[i] = __builtin_bswap64((uint64_t)(pool + P_SPPAGES + (uint32_t)i * 4096u));
    ((volatile uint64_t *)(pool + P_DCBAA))[0] = nsp ? __builtin_bswap64((uint64_t)(pool + P_SPARR)) : 0;
    ring_init(&cmd, pool + P_CMD, NTRB_CMD);
    evt = (volatile uint32_t *)(pool + P_EVT);
    evt_deq = 0; evt_ccs = 1;
    volatile uint32_t *erst = (volatile uint32_t *)(pool + P_ERST);
    erst[0] = LE32((uint32_t)(pool + P_EVT)); erst[1] = 0; erst[2] = LE32(NTRB_EVT); erst[3] = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    wr32(op + 0x38, (uint32_t)max_slots);                                          /* CONFIG */
    wr64(op + 0x30, (uint64_t)(pool + P_DCBAA));                                   /* DCBAAP */
    wr64(op + 0x18, (uint64_t)(pool + P_CMD) | 1u);                                /* CRCR, RCS */
    wr32(rt + 0x28, 1);                                                            /* ERSTSZ */
    wr64(rt + 0x38, (uint64_t)(pool + P_EVT));                                     /* ERDP */
    wr64(rt + 0x30, (uint64_t)(pool + P_ERST));                                    /* ERSTBA */
    wr32(rt + 0x20, 0);                                                            /* IMAN: polled */
    wr32(op, rd32(op) | 1u);                                                       /* Run */
    for (int i = 0; i < 100 && (rd32(op + 4) & 1u); i++) mdelay(1);
    if (rd32(op + 4) & 1u) { xlog("[FRF-XHCI] FAIL: controller would not run (USBSTS %08x)\n", rd32(op + 4)); return -1; }

    /* power every port */
    for (int p = 1; p <= max_ports; p++) {
        uintptr_t pr = op + 0x400u + 0x10u * (uint32_t)(p - 1);
        uint32_t v = rd32(pr);
        if (!(v & (1u << 9))) wr32(pr, (v & ((3u << 14) | (7u << 25))) | (1u << 9));
    }
    int cc = command(0, 0, 23u << 10, 0);                                          /* No-Op: the rings work */
    xlog("[FRF-XHCI] controller running, command ring %s\n", cc == 1 ? "OK" : "NOT answering");
    return cc == 1 ? 0 : -1;
}

/* ---- entry from CPU1 -------------------------------------------------------- */
int frf_xhci_wanted(void)
{
    if (bootarg_has("frf_usb=neptune")) { xlog("[FRF-XHCI] frf_usb=neptune: USB-A ports left to AmigaOS (xhci.device + Neptune)\n"); return 0; }
    int bcm2711 = tick_hz() > 20000000u, pcie = pcie_node() != 0;
    xlog("[FRF-XHCI] decision: BCM2711 %s, DMA pool %p, pcie0 %s -> %s\n", bcm2711 ? "yes" : "no",
            (void *)frf_dma_phys, pcie ? "found" : "MISSING",
            (bcm2711 && frf_dma_phys && pcie) ? "kernel drives the USB-A ports" : "NOT starting (FAIL)");
    return bcm2711 && frf_dma_phys && pcie;   /* a BCM2711 with PCIe */
}

static void ready(void)
{
    frf_boot_stage(10);
    __atomic_store_n(&frf_usb_boot_ready, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
}

extern void alice_net_poll(void);
void __attribute__((noreturn)) frf_xhci_main(void)
{
    pool = frf_dma_phys;
    xlog("[FRF-XHCI] Pi 4 USB-A: kernel xHCI driver (keyboard + mouse), DMA pool %p\n", (void *)pool);
    if (pcie_find()) goto dead;
    {   /* The kernel owns the PCIe link and the VL805 from here. Hide the
           pcie0 alias from AmigaOS: rondoval's bcmpcie.library looks the
           controller up by that alias, so an xhci.device started by mistake
           finds no controller instead of resetting the link under us. */
        of_node_t *al = dt_find_node("/aliases");
        of_property_t *ap = al ? dt_find_property(al, "pcie0") : 0;
        static char hidden[] = "alice68,pcie0-owned-by-kernel";
        if (ap) { ap->op_name = hidden; xlog("[FRF-XHCI] pcie0 hidden from AmigaOS (frf_usb=neptune gives it back)\n"); }
    }
    if (pcie_bringup() || pcie_enumerate()) goto dead;
    int rc_ = vl805_reload((volatile uint32_t *)(pool + P_MBOX));
    xlog("[FRF-XHCI] VL805 firmware reload: %s (%d)\n", rc_ ? "FAILED" : "done", rc_);
    mdelay(5);
    if (xhci_start()) goto dead;

    mdelay(100);                                   /* let the devices connect */
    for (int p = 1; p <= max_ports; p++) {
        port_pending[p] = 0;
        uint32_t v = rd32(op + 0x400u + 0x10u * (uint32_t)(p - 1));
        if (v & 1u) root_attach(p);
    }
    for (int s = 1; s <= MAX_SLOTS; s++)           /* the devices on the hub(s) */
        if (devs[s].used && devs[s].is_hub) hub_poll(s);
    xlog("[FRF-XHCI] boot scan done: %u keyboard(s), %u mouse/mice\n", n_kbd, n_mouse);
    frf_post_usb_devices(n_kbd, n_mouse, 0, 0);
    ready();

    uint64_t next_hub = now_ticks();
    for (;;) {
        struct evt e;
        while (evt_get(&e)) {
            int type = (int)((e.ctl >> 10) & 63u);
            if (type == 32) on_transfer(&e);
            else if (type == 34 && (e.p0 >> 24) < 256) port_pending[e.p0 >> 24] = 1;
        }
        for (int p = 1; p <= max_ports; p++)
            if (port_pending[p]) { port_pending[p] = 0; root_change(p); }
        if ((int64_t)(now_ticks() - next_hub) >= 0) {
            next_hub = now_ticks() + tick_hz() / 20u;      /* hubs: every 50 ms */
            for (int s = 1; s <= MAX_SLOTS; s++)
                if (devs[s].used && devs[s].is_hub) hub_poll(s);
        }
        pt_service();                              /* Neptune's devices (frf_xhci_pt.c) */
        alice_net_poll();                          /* the WiFi's turn (src/alice/alice_net.c) */
        if (frf_native_usb_reboot_requested()) {
            xlog("[FRF-XHCI] Ctrl+LeftShift+F11: host reboot\n");
            aga_reboot(0);
        }
        pt_amiga_reset(frf_native_usb_amiga_reset_requested());   /* Ctrl-Amiga-Amiga (frf_xhci_pt.c) */
        __asm__ volatile("yield");                 /* CPU1 is USB's alone: no sleeping, every request starts at once */
    }
dead:
    { extern volatile int frf_usb_failed; frf_usb_failed = 1; }
    xlog("[FRF-XHCI] FAIL: USB-A ports unavailable - use frf_usb=neptune and AmigaOS's xhci.device, or the USB-C port\n");
    ready();
    for (;;) { alice_net_poll(); __asm__ volatile("yield"); }     /* CPU1 still runs the network */
}
