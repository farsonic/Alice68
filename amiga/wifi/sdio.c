/* SPDX-License-Identifier: GPL-2.0-only
 * alice68wifi.device: SDIO on the Pi 4's Arasan SD host controller ("mmcnr",
 * bus 0x7e300000), by programmed I/O. Written from the SD Host Controller
 * Simplified Specification (register map, as in the BCM2835 peripherals
 * manual's "EMMC" chapter) and the SDIO Simplified Specification
 * (CMD5/CMD52/CMD53, CCCR and FBR).
 *
 * The controller takes 32-bit accesses only. Its registers are little-endian
 * (byte-swapped for the 68k); the data FIFO is a byte stream that a 32-bit
 * load moves in order, so FIFO words are copied as they are - unless chip.c
 * finds the chip id swapped, which sets h->swap. */
#include "bus.h"

#define R_BLKSIZECNT    0x04
#define R_ARG1          0x08
#define R_CMDTM         0x0c
#define R_RESP0         0x10
#define R_DATA          0x20
#define R_STATUS        0x24
#define R_CONTROL0      0x28
#define R_CONTROL1      0x2c
#define R_INTERRUPT     0x30
#define R_IRPT_MASK     0x34
#define R_IRPT_EN       0x38
#define R_SLOTISR_VER   0xfc

#define ST_CMD_INHIBIT  (1u << 0)
#define ST_DAT_INHIBIT  (1u << 1)

#define INT_CMD_DONE    (1u << 0)
#define INT_DATA_DONE   (1u << 1)
#define INT_WRITE_RDY   (1u << 4)
#define INT_READ_RDY    (1u << 5)
#define INT_CARD        (1u << 8)
#define INT_ERR         (1u << 15)
#define INT_ERRORS      0xffff0000u

#define C1_CLK_INTLEN   (1u << 0)
#define C1_CLK_STABLE   (1u << 1)
#define C1_CLK_EN       (1u << 2)
#define C1_SRST_HC      (1u << 24)
#define C1_SRST_CMD     (1u << 25)
#define C1_SRST_DATA    (1u << 26)

#define TM_BLKCNT_EN    (1u << 1)
#define TM_READ         (1u << 4)
#define TM_MULTI        (1u << 5)
#define CMD_RSP_NONE    (0u << 16)
#define CMD_RSP_48      (2u << 16)
#define CMD_CRCCHK      (1u << 19)
#define CMD_IXCHK       (1u << 20)
#define CMD_ISDATA      (1u << 21)
#define CMD_IDX(n)      ((uint32_t)(n) << 24)

#define R5_ERRORS       0xcb00u          /* COM_CRC, ILLEGAL, ERROR, FUNCTION, OUT_OF_RANGE */

static inline uint32_t swap32(uint32_t v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }
static inline uint32_t rd(struct sdhost *h, uint32_t off) { return swap32(h->regs[off / 4]); }
static inline void wr(struct sdhost *h, uint32_t off, uint32_t v)
{
    h->regs[off / 4] = swap32(v);
    if (h->slow) wifi_udelay(6);     /* two SD clocks between writes at 400 kHz */
}

static int wait_bits(struct sdhost *h, uint32_t off, uint32_t mask, int set, uint32_t us)
{
    uint32_t t0 = wifi_usecs();
    for (;;) {
        uint32_t v = rd(h, off);
        if (set ? (v & mask) : !(v & mask)) return 0;
        if (wifi_usecs() - t0 > us) return -1;
    }
}

/* wait for an interrupt status bit, or an error */
static int wait_int(struct sdhost *h, uint32_t bit, uint32_t us)
{
    uint32_t t0 = wifi_usecs();
    for (;;) {
        uint32_t v = rd(h, R_INTERRUPT);
        if (v & (INT_ERR | INT_ERRORS)) {
            h->last_int = v;
            wr(h, R_INTERRUPT, v & (INT_ERR | INT_ERRORS | bit));
            return -(int)(v >> 16 ? v >> 16 : 1);
        }
        if (v & bit) {
            wr(h, R_INTERRUPT, bit);
            return 0;
        }
        if (wifi_usecs() - t0 > us) { h->last_int = v; return -0x10000; }
    }
}

static void reset_lines(struct sdhost *h)
{
    wr(h, R_CONTROL1, rd(h, R_CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
    wait_bits(h, R_CONTROL1, C1_SRST_CMD | C1_SRST_DATA, 0, 10000);
    wr(h, R_INTERRUPT, 0xffffffffu & ~INT_CARD);
}

static int cmd(struct sdhost *h, unsigned idx, uint32_t arg, uint32_t flags, uint32_t *resp)
{
    if (wait_bits(h, R_STATUS, ST_CMD_INHIBIT | ((flags & CMD_ISDATA) ? ST_DAT_INHIBIT : 0), 0, 100000)) {
        h->errors++;
        reset_lines(h);
    }
    wr(h, R_INTERRUPT, 0xffffffffu & ~INT_CARD);
    wr(h, R_ARG1, arg);
    wr(h, R_CMDTM, CMD_IDX(idx) | flags);
    int e = wait_int(h, INT_CMD_DONE, 100000);
    if (e) {
        h->errors++;
        reset_lines(h);
        return e;
    }
    if (resp) *resp = rd(h, R_RESP0);
    return 0;
}

int sdh_set_clock(struct sdhost *h, uint32_t hz)
{
    uint32_t c1 = rd(h, R_CONTROL1) & ~(C1_CLK_EN | 0xffe0u);
    wr(h, R_CONTROL1, c1);
    uint32_t div, field, actual;
    if (h->spec >= 2) {                         /* 10-bit divided clock: base / (2 * div) */
        div = h->base_hz <= hz ? 0 : (h->base_hz + 2 * hz - 1) / (2 * hz);
        if (div > 0x3ff) div = 0x3ff;
        field = (div & 0xff) << 8 | ((div >> 8) & 3) << 6;
        actual = div ? h->base_hz / (2 * div) : h->base_hz;
    } else {                                    /* power-of-two divider */
        div = 1;
        while (div < 128 && h->base_hz / (2 * div) > hz) div <<= 1;
        field = div << 8;
        actual = h->base_hz / (2 * div);
    }
    c1 = (c1 & ~0x000f0000u) | 0x000e0000u | field | C1_CLK_INTLEN;   /* data timeout 2^27 clocks */
    h->slow = 1;                                /* until the clock runs */
    wr(h, R_CONTROL1, c1);
    if (wait_bits(h, R_CONTROL1, C1_CLK_STABLE, 1, 100000)) {
        wifi_log("SD clock did not stabilise\n");
        return -1;
    }
    wr(h, R_CONTROL1, c1 | C1_CLK_EN);
    h->sd_hz = actual;
    h->slow = actual < 1000000;
    wifi_udelay(2000);
    return 0;
}

void sdh_pins(void)
{
    volatile uint32_t *g = (volatile uint32_t *)SDH_GPIO_BASE;
    /* GPIO 34-39: ALT3 (SD1 CLK, CMD, DAT0-3) */
    uint32_t f = swap32(g[0x0c / 4]);
    for (int pin = 34; pin <= 39; pin++) {
        int s = (pin - 30) * 3;
        f = (f & ~(7u << s)) | (7u << s);
    }
    g[0x0c / 4] = swap32(f);
    /* BCM2711 pulls (GPIO_PUP_PDN_CNTRL_REG2 at 0xec, GPIO 32-47): CLK none, CMD/DAT up */
    uint32_t p = swap32(g[0xec / 4]);
    for (int pin = 34; pin <= 39; pin++) {
        int s = (pin - 32) * 2;
        p = (p & ~(3u << s)) | ((pin == 34 ? 0u : 1u) << s);
    }
    g[0xec / 4] = swap32(p);
}

int sdh_init(struct sdhost *h, uint32_t base_hz)
{
    h->regs = (volatile uint32_t *)SDH_BASE;
    h->base_hz = base_hz;
    h->slow = 1;
    h->spec = (uint8_t)(rd(h, R_SLOTISR_VER) >> 16);
    wifi_log("SD host: spec %u, input clock %u Hz\n", h->spec, base_hz);

    wr(h, R_CONTROL1, C1_SRST_HC);
    if (wait_bits(h, R_CONTROL1, C1_SRST_HC, 0, 100000)) { wifi_log("SD host reset timed out\n"); return -1; }
    wr(h, R_CONTROL0, 0x0f00);                  /* bus power on, 3.3 V, 1-bit, normal speed */
    wr(h, R_IRPT_EN, 0);
    wr(h, R_IRPT_MASK, 0xffffffffu);
    wr(h, R_INTERRUPT, 0xffffffffu);
    if (sdh_set_clock(h, 400000)) return -1;

    cmd(h, 0, 0, CMD_RSP_NONE, 0);              /* GO_IDLE */
    wifi_udelay(2000);

    uint32_t ocr = 0;
    if (cmd(h, 5, 0, CMD_RSP_48, &ocr)) { wifi_log("no SDIO card (CMD5)\n"); return -2; }
    uint32_t t0 = wifi_usecs();
    do {
        if (cmd(h, 5, ocr & 0x00ff8000u, CMD_RSP_48, &ocr)) return -3;
        if (wifi_usecs() - t0 > 1000000) { wifi_log("SDIO card not ready (OCR %08x)\n", ocr); return -4; }
    } while (!(ocr & 0x80000000u));
    wifi_log("SDIO card: %u functions, OCR %08x\n", (ocr >> 28) & 7, ocr);

    uint32_t r;
    if (cmd(h, 3, 0, CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK, &r)) { wifi_log("CMD3 failed\n"); return -5; }
    h->rca = (uint16_t)(r >> 16);
    if (cmd(h, 7, (uint32_t)h->rca << 16, CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK, &r)) { wifi_log("CMD7 failed\n"); return -6; }
    return 0;
}

int sdh_cmd52(struct sdhost *h, int write, unsigned fn, uint32_t addr, uint8_t *val)
{
    uint32_t r, arg = (write ? 1u << 31 : 0) | (fn & 7) << 28 | (addr & 0x1ffff) << 9 | (write ? *val : 0);
    int e = cmd(h, 52, arg, CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK, &r);
    if (e) return e;
    if (r & R5_ERRORS) { h->errors++; return -0x20000 - (int)((r >> 8) & 0xff); }
    if (!write) *val = (uint8_t)r;
    return 0;
}

int sdh_set_blksz(struct sdhost *h, unsigned fn, uint16_t size)
{
    uint32_t a = fn * 0x100 + 0x10;
    uint8_t lo = (uint8_t)size, hi = (uint8_t)(size >> 8);
    if (sdh_cmd52(h, 1, 0, a, &lo) || sdh_cmd52(h, 1, 0, a + 1, &hi)) return -1;
    h->blksz[fn] = size;
    return 0;
}

int sdh_bus4(struct sdhost *h, uint32_t max_hz)
{
    uint8_t v;
    if (sdh_cmd52(h, 0, 0, 0x07, &v)) return -1;
    v = (uint8_t)((v & ~3) | 2);                /* 4-bit bus */
    if (sdh_cmd52(h, 1, 0, 0x07, &v)) return -1;
    wr(h, R_CONTROL0, rd(h, R_CONTROL0) | 2);
    uint32_t hz = max_hz < 25000000 ? max_hz : 25000000;
    if (max_hz > 25000000 && !sdh_cmd52(h, 0, 0, 0x13, &v) && (v & 1)) {
        v |= 2;                                 /* EHS: high speed */
        if (!sdh_cmd52(h, 1, 0, 0x13, &v)) {
            wr(h, R_CONTROL0, rd(h, R_CONTROL0) | 4);
            hz = max_hz < 50000000 ? max_hz : 50000000;
        }
    }
    if (sdh_set_clock(h, hz)) return -1;
    wifi_log("SDIO: 4-bit bus, %u Hz\n", h->sd_hz);
    return 0;
}

static int xfer(struct sdhost *h, int write, unsigned fn, uint32_t addr, uint32_t *buf, uint32_t bs, uint32_t nblk, int block, int incr)
{
    uint32_t count = block ? nblk : (bs == 512 ? 0 : bs);
    uint32_t arg = (write ? 1u << 31 : 0) | (fn & 7) << 28 | (block ? 1u << 27 : 0) | (incr ? 1u << 26 : 0) |
                   (addr & 0x1ffff) << 9 | count;
    uint32_t tm = (write ? 0 : TM_READ) | (block ? TM_BLKCNT_EN : 0) | (nblk > 1 ? TM_MULTI : 0);
    wr(h, R_BLKSIZECNT, nblk << 16 | bs);
    uint32_t r;
    int e = cmd(h, 53, arg, CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK | CMD_ISDATA | tm, &r);
    if (e) return e;
    if (r & R5_ERRORS) {
        reset_lines(h);
        if (!(r & 0x4000)) {                    /* not "illegal command" (a sleeping chip) */
            h->errors++;
            wifi_log("CMD53 %s F%u %x: R5 flags %x\n", write ? "write" : "read", fn, addr, (r >> 8) & 0xff);
        }
        return -0x20000 - (int)((r >> 8) & 0xff);
    }
    for (uint32_t b = 0; b < nblk; b++) {
        if ((e = wait_int(h, write ? INT_WRITE_RDY : INT_READ_RDY, 500000))) goto fail;
        volatile uint32_t *fifo = &h->regs[R_DATA / 4];
        uint32_t n = bs / 4;
        if (write) {
            if (h->swap) while (n--) *fifo = swap32(*buf++);
            else while (n--) *fifo = *buf++;
        } else {
            if (h->swap) while (n--) *buf++ = swap32(*fifo);
            else while (n--) *buf++ = *fifo;
        }
    }
    if ((e = wait_int(h, INT_DATA_DONE, 500000))) goto fail;
    return 0;
fail:
    h->errors++;
    wifi_log("CMD53 %s F%u %x %s %u x %u: error %x, status %x, int %x\n", write ? "write" : "read", fn, addr,
             block ? "blocks" : "bytes", nblk, bs, (uint32_t)-e, rd(h, R_STATUS), h->last_int);
    reset_lines(h);
    return e;
}

static int cmd53_once(struct sdhost *h, int write, unsigned fn, uint32_t addr, void *buf, uint32_t len, int incr)
{
    uint32_t *p = buf, bs = h->blksz[fn];
    if (((uint32_t)(uintptr_t)buf & 3) || (len & 3)) return -0x30000;
    if (bs && len >= bs) {
        uint32_t nblk = len / bs;
        while (nblk) {
            uint32_t n = nblk > 511 ? 511 : nblk;
            int e = xfer(h, write, fn, addr, p, bs, n, 1, incr);
            if (e) return e;
            p += n * bs / 4;
            if (incr) addr += n * bs;
            nblk -= n;
            len -= n * bs;
        }
    }
    /* byte mode: at most the function's block size (and 512) per command */
    uint32_t most = bs && bs < 512 ? bs : 512;
    while (len) {
        uint32_t n = len > most ? most : len;
        int e = xfer(h, write, fn, addr, p, n, 1, 0, incr);
        if (e) return e;
        p += n / 4;
        if (incr) addr += n;
        len -= n;
    }
    return 0;
}

/* CCCR I/O abort: the card leaves a transfer that went wrong (Linux brcmf_sdiod_abort) */
void sdh_abort(struct sdhost *h, unsigned fn)
{
    uint8_t v = (uint8_t)fn;
    sdh_cmd52(h, 1, 0, 0x06, &v);
    reset_lines(h);
}

/* A failed transfer is aborted. Backplane transfers (F1: registers, RAM, the
   firmware download) are idempotent and are retried; when they keep failing
   the SD clock is halved, down to about 5 MHz. Frame transfers (F2) are not
   retried - the caller recovers the frame stream - but their failures count
   towards slowing down too. */
int sdh_cmd53(struct sdhost *h, int write, unsigned fn, uint32_t addr, void *buf, uint32_t len, int incr)
{
    for (int attempt = 0; ; attempt++) {
        int e = cmd53_once(h, write, fn, addr, buf, len, incr);
        if (!e) return 0;
        if (e == -0x30000) return e;            /* alignment: the caller's bug, not the line */
        if (e <= -0x20000 && ((-e - 0x20000) & 0x40)) return e;   /* illegal command: the chip is asleep (chip.c wakes it) */
        sdh_abort(h, fn);
        uint32_t now = wifi_usecs();
        if (now - h->errwin > 1000000) { h->errwin = now; h->nerr = 0; }
        if (++h->nerr >= 8 && h->sd_hz > 6000000) {     /* 8 errors within a second: slower */
            uint32_t to = h->sd_hz / 2;
            wifi_log("SDIO errors: slowing down to %u Hz\n", to);
            sdh_set_clock(h, to);
            h->nerr = 0;
        }
        if (fn != 1 || attempt >= 8) return e;
    }
}

void sdh_card_irq(struct sdhost *h, int enable)
{
    uint32_t v = rd(h, R_IRPT_EN);
    h->regs[R_IRPT_EN / 4] = swap32(enable ? v | INT_CARD : v & ~INT_CARD);
}

int sdh_card_irq_pending(struct sdhost *h)
{
    return (rd(h, R_INTERRUPT) & rd(h, R_IRPT_EN) & INT_CARD) != 0;
}
