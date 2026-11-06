/* SPDX-License-Identifier: GPL-2.0-only
 * alice68wifi.device: the SDIO host (Arasan SDHCI on the Pi 4, the "mmcnr"
 * controller at bus 0x7e300000 = $F2300000 for the 68k) and the Broadcom
 * chip behind it (backplane, cores, firmware download, clocks).
 *
 * sdio.c is written from the SD Host Controller and SDIO specifications;
 * chip.c follows OpenBSD's bwfm and Linux's brcmfmac (both ISC). */
#ifndef ALICE68_WIFI_BUS_H
#define ALICE68_WIFI_BUS_H
#include <stdint.h>

/* provided by the device (device.c) */
void wifi_udelay(uint32_t us);
uint32_t wifi_usecs(void);                     /* free-running microsecond counter */
void wifi_log(const char *fmt, ...);

/* ---- SD host ------------------------------------------------------------- */
#define SDH_BASE        0xF2300000u
#define SDH_GPIO_BASE   0xF2200000u

struct sdhost {
    volatile uint32_t *regs;
    uint32_t base_hz;       /* the controller's input clock */
    uint32_t sd_hz;         /* SD clock now */
    uint16_t rca;
    uint16_t blksz[3];      /* per function */
    uint8_t  spec;          /* SDHCI spec version (0 = 1.0, 2 = 3.0) */
    uint8_t  slow;          /* identification clock: space register writes */
    uint8_t  swap;          /* data FIFO words need a byte swap (detected) */
    uint8_t  nerr;          /* transfer errors in the current second */
    uint32_t errwin;        /* start of that second (wifi_usecs) */
    uint32_t errors;
    uint32_t last_int;      /* interrupt status at the last error */
};

void sdh_pins(void);                                        /* GPIO 34-39 to the SD host (ALT3) */
int  sdh_init(struct sdhost *h, uint32_t base_hz);          /* host reset, card identification, select */
int  sdh_set_clock(struct sdhost *h, uint32_t hz);
int  sdh_bus4(struct sdhost *h, uint32_t max_hz);          /* 4-bit bus, high speed if the card can */
int  sdh_cmd52(struct sdhost *h, int write, unsigned fn, uint32_t addr, uint8_t *val);
/* len a multiple of 4; block mode for whole blocks, byte mode for the rest */
int  sdh_cmd53(struct sdhost *h, int write, unsigned fn, uint32_t addr, void *buf, uint32_t len, int incr);
int  sdh_set_blksz(struct sdhost *h, unsigned fn, uint16_t size);
void sdh_abort(struct sdhost *h, unsigned fn);              /* CCCR I/O abort + line reset */
void sdh_card_irq(struct sdhost *h, int enable);            /* card interrupt to the GIC on/off */
int  sdh_card_irq_pending(struct sdhost *h);

/* ---- the chip --------------------------------------------------------------- */
#define CHIP_MAX_CORES  24
struct chip_core { uint16_t id; uint8_t rev; uint32_t base, wrap; };

struct chip {
    struct sdhost *h;
    uint32_t bar0;
    uint16_t id;
    uint8_t  rev;
    uint8_t  sr;            /* save/restore capable: HT clock is kept by the chip */
    uint32_t rambase, ramsize;
    uint32_t cc_caps, cc_caps_ext, pmurev;
    int      ncores;
    struct chip_core cores[CHIP_MAX_CORES];
    uint32_t sdio_base;     /* SDIO device core */
    uint32_t cc_base;       /* chipcommon */
    uint8_t *bounce;        /* 4-aligned transfer buffer, CHIP_BOUNCE bytes */
    uint32_t wakes;         /* times the chip was woken from save/restore sleep */
};
#define CHIP_BOUNCE     2048

uint8_t  chip_r8(struct chip *c, uint32_t addr);            /* F0 (< 0x800) or F1 register */
void     chip_w8(struct chip *c, uint32_t addr, uint8_t v);
uint32_t chip_r32(struct chip *c, uint32_t addr);           /* backplane address */
void     chip_w32(struct chip *c, uint32_t addr, uint32_t v);
int      chip_ram(struct chip *c, uint32_t addr, uint8_t *buf, uint32_t len, int write);

int  chip_attach(struct chip *c, struct sdhost *h);         /* identify, cores, RAM, halt the CPU */
void chip_card_int(struct chip *c, int on);                 /* CCCR interrupt enable */
int  chip_wake(struct chip *c);                             /* out of save/restore sleep (KSO) */
int  chip_load(struct chip *c, const uint8_t *fw, uint32_t fwlen, const uint8_t *nv, uint32_t nvlen);  /* download, start, F2 */
int  chip_irq_ack(struct chip *c, uint32_t *intstatus, uint32_t *mailbox);
/* frames on F2 */
int  chip_frame(struct chip *c, void *buf, uint32_t len, int write);

/* SDIO core registers (offsets from sdio_base) */
#define SD_INTSTATUS        0x020
#define SD_HOSTINTMASK      0x024
#define SD_TOSBMAILBOX      0x040
#define SD_TOSBMAILBOXDATA  0x048
#define SD_TOHOSTMAILBOXDATA 0x04c
#define I_HMB_FC_STATE      (1 << 4)
#define I_HMB_FC_CHANGE     (1 << 5)
#define I_HMB_FRAME_IND     (1 << 6)
#define I_HMB_HOST_INT      (1 << 7)
#define I_HMB_SW_MASK       0xf0
#define I_CHIPACTIVE        (1u << 29)
#define HMB_DATA_NAKHANDLED 0x01
#define HMB_DATA_DEVREADY   0x02
#define HMB_DATA_FC         0x04
#define HMB_DATA_FWREADY    0x08
#define HMB_DATA_FWHALT     0x10
#define SMB_INT_ACK         0x02

#endif
