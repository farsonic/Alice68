/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 kernel WiFi: the Broadcom/Cypress chip over SDIO - backplane access,
 * core enumeration (EROM), the ARM CR4 core halt/release, firmware and NVRAM
 * download, clocks and function 2. The sequence follows OpenBSD's
 * if_bwfm_sdio.c/bwfm.c and Linux's brcmfmac sdio.c/chip.c (ISC); the 43455's
 * F2 watermark values are Linux's. */
#include "bus.h"

void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);

/* F1 registers */
#define F1_WATERMARK        0x10008
#define F1_DEVICE_CTL       0x10009
#define  DEVCTL_F2WM_ENAB       0x10
#define F1_SBADDRLOW        0x1000a
#define F1_SBADDRMID        0x1000b
#define F1_SBADDRHIGH       0x1000c
#define F1_CHIPCLKCSR       0x1000e
#define  CLK_FORCE_ALP          0x01
#define  CLK_FORCE_HT           0x02
#define  CLK_ALP_AVAIL_REQ      0x08
#define  CLK_HT_AVAIL_REQ       0x10
#define  CLK_FORCE_HW_CLKREQ_OFF 0x20
#define  CLK_ALP_AVAIL          0x40
#define  CLK_HT_AVAIL           0x80
#define F1_SDIOPULLUP       0x1000f
#define F1_MESBUSYCTRL      0x1001d
#define F1_WAKEUPCTRL       0x1001e
#define F1_SLEEPCSR         0x1001f
#define  SLEEPCSR_KSO           0x01
/* CCCR */
#define CCCR_IOEN           0x02
#define CCCR_IORDY          0x03
#define CCCR_INTEN          0x04
#define CCCR_BRCM_CARDCAP   0xf0
#define CCCR_BRCM_CARDCTRL  0xf1
#define  CARDCTRL_WLANRESET     0x02

#define SB_OFT_MASK         0x7fff
#define SB_PAGE             0x8000
#define SB_ACCESS_4B        0x8000

/* backplane */
#define CHIP_BASE           0x18000000u
#define CC_CHIPID           0x000
#define CC_CAPS             0x004
#define CC_CAPS_EXT         0x0ac
#define CC_EROMPTR          0x0fc
#define CC_PMUCONTROL       0x600
#define CC_PMUCAPS          0x604
#define CC_CHIPCONTROL_ADDR 0x650
#define CC_CHIPCONTROL_DATA 0x654
#define AGENT_IOCTL         0x408
#define  IOCTL_CLK              0x0001
#define  IOCTL_FGC              0x0002
#define  IOCTL_CR4_CPUHALT      0x0020
#define  IOCTL_D11_PHYCLOCKEN   0x0004
#define  IOCTL_D11_PHYRESET     0x0008
#define AGENT_RESET_CTL     0x800
#define CORE_CHIPCOMMON     0x800
#define CORE_INTERNAL_MEM   0x80e
#define CORE_80211          0x812
#define CORE_PMU            0x827
#define CORE_SDIO_DEV       0x829
#define CORE_ARM_CR4        0x83e
#define CORE_GCI            0x840
#define CR4_CAP             0x004
#define CR4_BANKIDX         0x040
#define CR4_BANKINFO        0x044

/* ---- register access ------------------------------------------------------ */
uint8_t chip_r8(struct chip *c, uint32_t addr)
{
    uint8_t v = 0;
    sdh_cmd52(c->h, 0, (addr & ~0x7ffu) ? 1 : 0, addr, &v);
    return v;
}

void chip_w8(struct chip *c, uint32_t addr, uint8_t v)
{
    sdh_cmd52(c->h, 1, (addr & ~0x7ffu) ? 1 : 0, addr, &v);
}

static void window(struct chip *c, uint32_t addr)
{
    uint32_t bar0 = addr & ~(uint32_t)SB_OFT_MASK;
    if (bar0 == c->bar0) return;
    chip_w8(c, F1_SBADDRLOW, (uint8_t)((bar0 >> 8) & 0x80));
    chip_w8(c, F1_SBADDRMID, (uint8_t)(bar0 >> 16));
    chip_w8(c, F1_SBADDRHIGH, (uint8_t)(bar0 >> 24));
    c->bar0 = bar0;
}

/* the chip is little-endian: a backplane word arrives as 4 bytes in order */
static uint32_t le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Wake a chip whose save/restore engine put it to sleep: while asleep it takes
   only the KSO write and answers everything else "illegal command". As Linux's
   brcmf_sdio_kso_control: write KSO, then read back until KSO and DEVON are set. */
int chip_wake(struct chip *c)
{
    uint8_t v = SLEEPCSR_KSO;
    sdh_cmd52(c->h, 1, 1, F1_SLEEPCSR, &v);
    wifi_udelay(2500);
    for (int i = 0; i < 64; i++) {
        uint8_t r = 0;
        if (!sdh_cmd52(c->h, 0, 1, F1_SLEEPCSR, &r) && (r & 3) == 3) { c->wakes++; return 0; }
        wifi_udelay(50);
        v = SLEEPCSR_KSO;
        sdh_cmd52(c->h, 1, 1, F1_SLEEPCSR, &v);
    }
    wifi_log("the chip does not wake up\n");
    return -1;
}

static int asleep(int e) { return e <= -0x20000 && e > -0x30000 && ((-e - 0x20000) & 0x40); }

static int f1_xfer(struct chip *c, int write, uint32_t addr, uint8_t *buf, uint32_t len)
{
    int e = sdh_cmd53(c->h, write, 1, (addr & SB_OFT_MASK) | SB_ACCESS_4B, buf, len, 1);
    if (asleep(e) && !chip_wake(c)) e = sdh_cmd53(c->h, write, 1, (addr & SB_OFT_MASK) | SB_ACCESS_4B, buf, len, 1);
    return e;
}

uint32_t chip_r32(struct chip *c, uint32_t addr)
{
    window(c, addr);
    if (f1_xfer(c, 0, addr, c->bounce, 4)) return 0xffffffffu;
    return le(c->bounce);
}

void chip_w32(struct chip *c, uint32_t addr, uint32_t v)
{
    window(c, addr);
    c->bounce[0] = (uint8_t)v; c->bounce[1] = (uint8_t)(v >> 8); c->bounce[2] = (uint8_t)(v >> 16); c->bounce[3] = (uint8_t)(v >> 24);
    f1_xfer(c, 1, addr, c->bounce, 4);
}

int chip_ram(struct chip *c, uint32_t addr, uint8_t *buf, uint32_t len, int write)
{
    while (len) {
        window(c, addr);
        uint32_t off = addr & SB_OFT_MASK, n = SB_PAGE - off;
        if (n > len) n = len;
        if (n > CHIP_BOUNCE) n = CHIP_BOUNCE;
        uint32_t n4 = (n + 3) & ~3u;
        if (write) {
            memcpy(c->bounce, buf, n);
            memset(c->bounce + n, 0, n4 - n);
        }
        int e = f1_xfer(c, write, off, c->bounce, n4);
        if (e) return e;
        if (!write) memcpy(buf, c->bounce, n);
        addr += n; buf += n; len -= n;
    }
    return 0;
}

/* ---- cores ------------------------------------------------------------------ */
static struct chip_core *core(struct chip *c, uint16_t id, int idx)
{
    for (int i = 0; i < c->ncores; i++)
        if (c->cores[i].id == id && idx-- == 0) return &c->cores[i];
    return 0;
}

static void ai_disable(struct chip *c, struct chip_core *k, uint32_t prereset, uint32_t reset)
{
    if (!(chip_r32(c, k->wrap + AGENT_RESET_CTL) & 1)) {
        chip_w32(c, k->wrap + AGENT_IOCTL, prereset | IOCTL_FGC | IOCTL_CLK);
        chip_r32(c, k->wrap + AGENT_IOCTL);
        chip_w32(c, k->wrap + AGENT_RESET_CTL, 1);
        wifi_udelay(20);
        for (int i = 300; i > 0 && chip_r32(c, k->wrap + AGENT_RESET_CTL) != 1; i--) wifi_udelay(10);
    }
    chip_w32(c, k->wrap + AGENT_IOCTL, reset | IOCTL_FGC | IOCTL_CLK);
    chip_r32(c, k->wrap + AGENT_IOCTL);
}

static void ai_reset(struct chip *c, struct chip_core *k, uint32_t prereset, uint32_t reset, uint32_t postreset)
{
    ai_disable(c, k, prereset, reset);
    for (int i = 50; i > 0; i--) {
        if (!(chip_r32(c, k->wrap + AGENT_RESET_CTL) & 1)) break;
        chip_w32(c, k->wrap + AGENT_RESET_CTL, 0);
        wifi_udelay(60);
    }
    chip_w32(c, k->wrap + AGENT_IOCTL, postreset | IOCTL_CLK);
    chip_r32(c, k->wrap + AGENT_IOCTL);
}

/* the address of a component's first 4K/8K slave port and its wrapper (OpenBSD's dmp_get_regaddr) */
static int erom_regaddr(struct chip *c, uint32_t *erom, uint32_t *base, uint32_t *wrap)
{
    uint32_t v = chip_r32(c, *erom);
    unsigned type = v & 0xf, wraptype;
    *base = *wrap = 0;
    if (type == 0x3) { wraptype = 3; *erom += 4; }               /* master port: master wrapper */
    else if ((type & ~8u) == 0x5) wraptype = 2;                  /* address: slave wrapper */
    else return 1;
    for (int guard = 0; guard < 64 && (*base == 0 || *wrap == 0); guard++) {
        int inner = 0;
        do {
            if (++inner > 64) return 1;
            v = chip_r32(c, *erom);
            type = v & 0xf;
            if (type == 0x1) return 0;                           /* next component */
            if (type == 0xf) return 1;                           /* end of table */
            *erom += 4;
        } while ((type & ~8u) != 0x5);
        if (type & 8) *erom += 4;                                /* 64-bit address: skip the high word */
        unsigned sz = (v >> 4) & 3;
        if (sz == 3) {                                           /* size given by a descriptor */
            uint32_t s = chip_r32(c, *erom);
            *erom += (s & 8) ? 8 : 4;
        }
        if (sz != 0 && sz != 1) continue;                        /* only 4K and 8K ports */
        unsigned st = (v >> 6) & 3;
        if (*base == 0 && st == 0) *base = v & 0xfffff000u;
        if (*wrap == 0 && st == wraptype) *wrap = v & 0xfffff000u;
    }
    return 0;
}

static void erom_scan(struct chip *c)
{
    uint32_t erom = chip_r32(c, CHIP_BASE + CC_EROMPTR);
    unsigned type = 0;
    for (int guard = 0; guard < 512 && type != 0xf; guard++) {
        uint32_t v = chip_r32(c, erom);
        type = v & 0xf;
        erom += 4;
        if (type != 0x1) continue;
        uint16_t id = (uint16_t)((v >> 8) & 0xfff);
        v = chip_r32(c, erom);
        type = v & 0xf;
        erom += 4;
        if (type != 0x1) return;
        unsigned nmw = (v >> 14) & 0x1f, nsw = (v >> 19) & 0x1f, rev = v >> 24;
        if (nmw + nsw == 0 && id != CORE_PMU && id != CORE_GCI) continue;
        uint32_t base, wrap;
        if (erom_regaddr(c, &erom, &base, &wrap)) continue;
        if (c->ncores < CHIP_MAX_CORES) {
            struct chip_core *k = &c->cores[c->ncores++];
            k->id = id; k->rev = (uint8_t)rev; k->base = base; k->wrap = wrap;
        }
    }
}

static void cr4_passive(struct chip *c)
{
    struct chip_core *k = core(c, CORE_ARM_CR4, 0);
    if (!k) return;
    uint32_t v = chip_r32(c, k->wrap + AGENT_IOCTL);
    ai_reset(c, k, v & IOCTL_CR4_CPUHALT, IOCTL_CR4_CPUHALT, IOCTL_CR4_CPUHALT);
    for (int i = 0; (k = core(c, CORE_80211, i)); i++)
        ai_disable(c, k, IOCTL_D11_PHYRESET | IOCTL_D11_PHYCLOCKEN, IOCTL_D11_PHYCLOCKEN);
}

static uint32_t pmu_base(struct chip *c)
{
    struct chip_core *cc = core(c, CORE_CHIPCOMMON, 0), *pmu = core(c, CORE_PMU, 0);
    if (cc && cc->rev >= 35 && (c->cc_caps_ext & 0x40) && pmu) return pmu->base;
    return c->cc_base;
}

static int plausible(uint32_t v) { uint32_t id = v & 0xffff; return ((v >> 28) & 0xf) == 1 && id >= 0x4000 && id < 0xb000; }

int chip_attach(struct chip *c, struct sdhost *h)
{
    c->h = h;
    c->bar0 = 0xffffffffu;
    c->ncores = 0;

    /* enable function 1 */
    uint8_t v = 2;
    if (sdh_cmd52(h, 1, 0, CCCR_IOEN, &v)) return -1;
    uint32_t t0 = wifi_usecs();
    do {
        if (sdh_cmd52(h, 0, 0, CCCR_IORDY, &v)) return -2;
        if (wifi_usecs() - t0 > 500000) { wifi_log("function 1 not ready\n"); return -3; }
    } while (!(v & 2));
    sdh_set_blksz(h, 0, 64);
    sdh_set_blksz(h, 1, 64);
    sdh_set_blksz(h, 2, 512);

    /* ALP clock (OpenBSD buscore_prepare) */
    chip_w8(c, F1_CHIPCLKCSR, CLK_ALP_AVAIL_REQ | CLK_FORCE_HW_CLKREQ_OFF);
    t0 = wifi_usecs();
    while (!(chip_r8(c, F1_CHIPCLKCSR) & (CLK_ALP_AVAIL | CLK_HT_AVAIL)))
        if (wifi_usecs() - t0 > 100000) { wifi_log("no ALP clock (%02x)\n", chip_r8(c, F1_CHIPCLKCSR)); return -4; }
    chip_w8(c, F1_CHIPCLKCSR, CLK_FORCE_HW_CLKREQ_OFF | CLK_FORCE_ALP);
    wifi_udelay(65);
    chip_w8(c, F1_SDIOPULLUP, 0);

    /* chip id - and which way round the SD host's data words come */
    uint32_t id = chip_r32(c, CHIP_BASE + CC_CHIPID);
    if (!plausible(id)) {
        h->swap = 1;
        uint32_t id2 = chip_r32(c, CHIP_BASE + CC_CHIPID);
        if (!plausible(id2)) {
            h->swap = 0;
            wifi_log("chip id unreadable: %08x\n", id);
            return -5;
        }
        id = id2;
        wifi_log("SD data words need swapping\n");
    }
    c->id = (uint16_t)id;
    c->rev = (uint8_t)((id >> 16) & 0xf);
    wifi_log("chip %x rev %u\n", c->id, c->rev);

    erom_scan(c);
    struct chip_core *k;
    if (!(k = core(c, CORE_CHIPCOMMON, 0)) || !core(c, CORE_SDIO_DEV, 0) || !core(c, CORE_ARM_CR4, 0)) {
        wifi_log("cores missing (%d found): not a CR4 FullMAC chip\n", c->ncores);
        return -6;
    }
    c->cc_base = k->base;
    c->sdio_base = core(c, CORE_SDIO_DEV, 0)->base;

    cr4_passive(c);

    /* RAM: banks of the CR4's tightly-coupled memory */
    k = core(c, CORE_ARM_CR4, 0);
    uint32_t cap = chip_r32(c, k->base + CR4_CAP), nb = (cap & 0xf) + ((cap >> 4) & 0xf), size = 0;
    for (uint32_t i = 0; i < nb; i++) {
        chip_w32(c, k->base + CR4_BANKIDX, i);
        uint32_t bi = chip_r32(c, k->base + CR4_BANKINFO);
        size += ((bi & 0x7f) + 1) * ((bi & 0x200) ? 1024 : 8192);
    }
    c->ramsize = size;
    c->rambase = c->id == 0x4345 ? 0x198000 : 0x180000;

    c->cc_caps = chip_r32(c, c->cc_base + CC_CAPS);
    c->cc_caps_ext = chip_r32(c, c->cc_base + CC_CAPS_EXT);
    uint32_t pmu = pmu_base(c);
    if (c->cc_caps & 0x10000000u) c->pmurev = chip_r32(c, pmu + CC_PMUCAPS) & 0xff;
    wifi_log("RAM %u KB at %x, %d cores, PMU rev %u\n", c->ramsize / 1024, c->rambase, c->ncores, c->pmurev);

    if (core(c, CORE_SDIO_DEV, 0)->rev >= 12) {
        v = chip_r8(c, F1_SLEEPCSR);
        if (!(v & SLEEPCSR_KSO)) chip_w8(c, F1_SLEEPCSR, v | SLEEPCSR_KSO);
    }
    chip_w8(c, CCCR_BRCM_CARDCTRL, chip_r8(c, CCCR_BRCM_CARDCTRL) | CARDCTRL_WLANRESET);
    chip_w32(c, pmu + CC_PMUCONTROL, chip_r32(c, pmu + CC_PMUCONTROL) | (2u << 13));   /* resource reload */
    v = 2;                                                       /* function 2 off */
    sdh_cmd52(h, 1, 0, CCCR_IOEN, &v);
    chip_w8(c, F1_CHIPCLKCSR, 0);

    /* save/restore: the 4345 reports it in chip control 3, bit 2 */
    c->sr = 0;
    if (c->pmurev >= 17 && c->id == 0x4345) {
        chip_w32(c, pmu + CC_CHIPCONTROL_ADDR, 3);
        c->sr = (chip_r32(c, pmu + CC_CHIPCONTROL_DATA) & 4) != 0;
    }
    return 0;
}

static int clock_wait(struct chip *c, uint8_t req, uint8_t avail, uint32_t us)
{
    chip_w8(c, F1_CHIPCLKCSR, req);
    uint32_t t0 = wifi_usecs();
    while (!(chip_r8(c, F1_CHIPCLKCSR) & avail))
        if (wifi_usecs() - t0 > us) return -1;
    return 0;
}

static int verify(struct chip *c, uint32_t addr, const uint8_t *want, uint32_t len)
{
    uint8_t tmp[256];
    while (len) {
        uint32_t n = len > sizeof tmp ? sizeof tmp : len;
        if (chip_ram(c, addr, tmp, n, 0)) return -1;
        for (uint32_t i = 0; i < n; i++)
            if (tmp[i] != want[i]) { wifi_log("verify failed at %x\n", addr + i); return -2; }
        addr += n; want += n; len -= n;
    }
    return 0;
}

/* CCCR interrupt enable: master, F1, F2. Off while polling: the chip would
   otherwise drive DAT1 between transfers. */
void chip_card_int(struct chip *c, int on)
{
    uint8_t v = on ? 0x07 : 0x00;
    sdh_cmd52(c->h, 1, 0, CCCR_INTEN, &v);
}

int chip_load(struct chip *c, const uint8_t *fw, uint32_t fwlen, const uint8_t *nv, uint32_t nvlen)
{
    if (clock_wait(c, CLK_ALP_AVAIL_REQ, CLK_ALP_AVAIL, 100000)) { wifi_log("no ALP clock for the download\n"); return -1; }
    uint32_t t0 = wifi_usecs();
    if (chip_ram(c, c->rambase, (uint8_t *)fw, fwlen, 1) || verify(c, c->rambase, fw, fwlen)) {
        wifi_log("firmware download failed\n");
        return -2;
    }
    uint32_t nvaddr = c->rambase + c->ramsize - nvlen;
    if (chip_ram(c, nvaddr, (uint8_t *)nv, nvlen, 1) || verify(c, nvaddr, nv, nvlen)) {
        wifi_log("NVRAM download failed\n");
        return -3;
    }
    wifi_log("firmware %u bytes + NVRAM %u bytes loaded in %u ms\n", fwlen, nvlen, (wifi_usecs() - t0) / 1000);

    /* start the CPU at the firmware's reset vector */
    chip_w32(c, c->sdio_base + SD_INTSTATUS, 0xffffffffu);
    uint8_t rv[4];
    memcpy(rv, fw, 4);
    chip_ram(c, 0, rv, 4, 1);
    ai_reset(c, core(c, CORE_ARM_CR4, 0), IOCTL_CR4_CPUHALT, 0, 0);
    chip_w8(c, F1_CHIPCLKCSR, 0);

    if (clock_wait(c, CLK_HT_AVAIL_REQ, CLK_HT_AVAIL, 5000000)) { wifi_log("no HT clock: firmware did not start\n"); return -4; }
    uint8_t clk = chip_r8(c, F1_CHIPCLKCSR);
    chip_w8(c, F1_CHIPCLKCSR, clk | CLK_FORCE_HT);

    /* function 2: the frame channel */
    chip_w32(c, c->sdio_base + SD_TOSBMAILBOXDATA, 4u << 16);     /* SDPCM protocol version 4 */
    uint8_t v = 2 | 4;
    sdh_cmd52(c->h, 1, 0, CCCR_IOEN, &v);
    t0 = wifi_usecs();
    do {
        if (sdh_cmd52(c->h, 0, 0, CCCR_IORDY, &v)) return -5;
        if (wifi_usecs() - t0 > 3000000) { wifi_log("function 2 not ready: firmware did not come up\n"); return -6; }
    } while (!(v & 4));
    wifi_log("firmware running (%u ms)\n", (wifi_usecs() - t0) / 1000);

    chip_w32(c, c->sdio_base + SD_HOSTINTMASK, I_HMB_SW_MASK | I_CHIPACTIVE);
    if (c->id == 0x4345) {
        chip_w8(c, F1_WATERMARK, 0x60);
        chip_w8(c, F1_DEVICE_CTL, chip_r8(c, F1_DEVICE_CTL) | DEVCTL_F2WM_ENAB);
        chip_w8(c, F1_MESBUSYCTRL, 0x50 | 0x80);
    } else
        chip_w8(c, F1_WATERMARK, 8);
    if (c->sr) {
        chip_w8(c, F1_WAKEUPCTRL, chip_r8(c, F1_WAKEUPCTRL) | 2);   /* HT wait */
        chip_w8(c, CCCR_BRCM_CARDCAP, 0x06);                        /* CMD14 support */
        chip_w8(c, F1_CHIPCLKCSR, CLK_FORCE_HT);
    } else
        chip_w8(c, F1_CHIPCLKCSR, clk);
    chip_card_int(c, 0);                    /* the chip's interrupt on DAT1 only when it is used */
    return 0;
}

/* 0 = read; a failed read (all ones) is never taken for status bits */
int chip_irq_ack(struct chip *c, uint32_t *intstatus, uint32_t *mailbox)
{
    *intstatus = *mailbox = 0;
    uint32_t raw = chip_r32(c, c->sdio_base + SD_INTSTATUS);
    if (raw == 0xffffffffu) return -1;
    uint32_t s = raw & (I_HMB_SW_MASK | I_CHIPACTIVE);
    if (s) chip_w32(c, c->sdio_base + SD_INTSTATUS, s);
    if (s & I_HMB_HOST_INT) {
        uint32_t m = chip_r32(c, c->sdio_base + SD_TOHOSTMAILBOXDATA);
        chip_w32(c, c->sdio_base + SD_TOSBMAILBOX, SMB_INT_ACK);
        if (m == 0xffffffffu) return -1;
        *mailbox = m;
    }
    *intstatus = s;
    return 0;
}

int chip_frame(struct chip *c, void *buf, uint32_t len, int write)
{
    window(c, c->cc_base);
    int e = sdh_cmd53(c->h, write, 2, (c->cc_base & SB_OFT_MASK) | SB_ACCESS_4B, buf, len, write);
    if (asleep(e) && !chip_wake(c)) e = sdh_cmd53(c->h, write, 2, (c->cc_base & SB_OFT_MASK) | SB_ACCESS_4B, buf, len, write);
    return e;
}
