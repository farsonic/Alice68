/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68's status page: what the ARM side is doing, for monitors on the 68k (PrismMonitor,
 * Alice68-Status). Read-only for the 68k and polled: nothing calls into the kernel.
 *
 * Where: the Emu68 ROM board (src/boards/emu68rom.c) maps one 4 KB page of kernel memory into
 * its own Zorro III space, read-only, at board base + ALICE68_STATUS_BOARD_OFF. The 68k finds
 * the address in devicetree.resource: node "/alice68", property "status" (one 32-bit word),
 * and "status-version" (ALICE68_STATUS_VERSION). The board is configured by Kickstart, so the
 * page is there from the start of AmigaOS.
 *
 * Updated about once a second by CPU1 (the USB/network core), when its loop runs: with
 * frf_usb=neptune CPU1 runs other code and the page stays as it was (seq does not move).
 *
 * Reading: seq is odd while the kernel writes. Read seq, then the fields, then seq again; if
 * the two differ or are odd, read again. Every field is a big-endian 32-bit word (the 68k's
 * own order) or a NUL-terminated string. ALICE68_ST_UNKNOWN (0xffffffff) means "not measured
 * on this machine" - never a guess. Counters wrap at 32 bits. New fields only ever go at the
 * end, with size telling how much there is; the version changes only if a field's meaning does.
 *
 * Shared by the kernel and the 68k side (amiga/status.c): 32-bit fields only.
 */
#ifndef ALICE68_STATUS_H
#define ALICE68_STATUS_H

#define ALICE68_STATUS_MAGIC     0x41363853u    /* 'A68S' */
#define ALICE68_STATUS_VERSION   1u
#define ALICE68_STATUS_BOARD_OFF 0x00700000u    /* in the Emu68 ROM board's 8 MB, past its modules */
#define ALICE68_ST_UNKNOWN       0xffffffffu

/* core roles */
#define ALICE68_ROLE_JIT      1u                /* the 68k: Emu68's JIT and the code it made */
#define ALICE68_ROLE_IO       2u                /* USB (xHCI), the network board, WiFi, Ethernet, Bluetooth */
#define ALICE68_ROLE_DISPLAY  3u                /* the HDMI output (HVS) */
#define ALICE68_ROLE_CHIPSET  4u                /* the AGA chipset, audio */

/* flags */
#define ALICE68_STF_MBOX_SENSORS 1u             /* clock, voltage, throttling are read (status.sensors=mbox) */

struct alice68_status_core {
    unsigned int role;          /* ALICE68_ROLE_* */
    unsigned int busy;          /* hundredths of a percent (0..10000), or ALICE68_ST_UNKNOWN */
    char         name[24];
};

struct alice68_status_net {     /* the network board's units, as alice68net.device sees them */
    unsigned int kind;          /* 0 none, 1 WiFi, 2 Ethernet (alice_net.h AN_KIND_*) */
    unsigned int link;          /* 1: frames can flow */
    unsigned int rx_pkts, tx_pkts, rx_bytes, tx_bytes, rx_drop, tx_err;
};

struct alice68_status {
    unsigned int magic;         /* 0x000 ALICE68_STATUS_MAGIC */
    unsigned int version;       /* 0x004 ALICE68_STATUS_VERSION */
    unsigned int size;          /* 0x008 bytes of this structure the kernel fills */
    unsigned int seq;           /* 0x00c odd while being written */
    unsigned int uptime_ms;     /* 0x010 since the kernel started, at this update */
    unsigned int interval_ms;   /* 0x014 between updates (about) */
    unsigned int flags;         /* 0x018 ALICE68_STF_* */
    unsigned int reserved0;     /* 0x01c */
    char         build[96];     /* 0x020 "Alice68 0.6.0 (15.11.2026) git:abcdef0, based on Emu68 ..." */
    struct alice68_status_core core[4];          /* 0x080 ARM cores 0-3 */
    /* sensors: temperature from the SoC's own register (no mailbox), the rest from the
       VideoCore mailbox only with status.sensors=mbox in cmdline.txt (see README) */
    unsigned int temp_mc;       /* 0x100 SoC temperature, thousandths of a degree C */
    unsigned int arm_hz;        /* 0x104 ARM clock now (mailbox) */
    unsigned int core_uv;       /* 0x108 core voltage, microvolts (mailbox) */
    unsigned int throttled;     /* 0x10c the firmware's GET_THROTTLED bits (mailbox) */
    unsigned int sensors_ms;    /* 0x110 uptime_ms when the mailbox values were read, 0: never */
    unsigned int reserved1[3];  /* 0x114 */
    /* Emu68's JIT (M68KState) */
    unsigned int jit_total;     /* 0x120 translation cache, bytes */
    unsigned int jit_free;      /* 0x124 free in it, bytes (as of the last translation or flush) */
    unsigned int jit_units;     /* 0x128 translated blocks in it now */
    unsigned int jit_translations; /* 0x12c blocks translated since boot (cache misses) */
    unsigned int jit_flushes;   /* 0x130 ALICE68_ST_UNKNOWN: not counted by Emu68 */
    unsigned int jit_hits;      /* 0x134 ALICE68_ST_UNKNOWN: not counted by Emu68 */
    unsigned int reserved2[2];  /* 0x138 */
    struct alice68_status_net net[2];            /* 0x140 unit 0 WiFi, unit 1 Ethernet */
    unsigned int usb_in_bytes;  /* 0x180 ALICE68_ST_UNKNOWN: not counted yet */
    unsigned int usb_out_bytes; /* 0x184 ALICE68_ST_UNKNOWN */
    unsigned int sd_read_blocks;  /* 0x188 ALICE68_ST_UNKNOWN: the SD card is driven from the 68k */
    unsigned int sd_write_blocks; /* 0x18c ALICE68_ST_UNKNOWN */
    unsigned int reserved3[4];  /* 0x190 */
};                              /* 0x1a0 */

_Static_assert(sizeof(struct alice68_status_core) == 32, "core");
_Static_assert(sizeof(struct alice68_status_net) == 32, "net");
_Static_assert(__builtin_offsetof(struct alice68_status, build) == 0x020, "build");
_Static_assert(__builtin_offsetof(struct alice68_status, core) == 0x080, "core");
_Static_assert(__builtin_offsetof(struct alice68_status, temp_mc) == 0x100, "temp");
_Static_assert(__builtin_offsetof(struct alice68_status, jit_total) == 0x120, "jit");
_Static_assert(__builtin_offsetof(struct alice68_status, net) == 0x140, "net");
_Static_assert(__builtin_offsetof(struct alice68_status, usb_in_bytes) == 0x180, "usb");
_Static_assert(sizeof(struct alice68_status) == 0x1a0, "size");

#endif
