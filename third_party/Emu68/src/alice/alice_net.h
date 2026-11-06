/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The Alice68 network board - shared by the kernel (alice_net.c, ARM,
 * big-endian) and alice68net.device (68k, big-endian). Same byte order on
 * both sides; every field is 32 bits or an array whose size is a multiple of
 * 4, so the 68k gcc (which aligns 32-bit fields to 2) and the ARM gcc lay the
 * structures out the same. Static asserts on both sides check it.
 *
 * The kernel runs the network hardware on CPU1 (the Pi 4's WiFi now; unit 1
 * is kept for the kernel's own Ethernet driver) and AmigaOS sees a 128 KB
 * Zorro II I/O board, manufacturer 0x6d73, product 0x6b, uncached on both
 * sides. Commands go into a unit's command box; frames go through slots in
 * the board; the kernel raises the Amiga's level-2 (PORTS) interrupt with
 * hdr.irq set when it has something new.
 */
#ifndef ALICE68_NET_H
#define ALICE68_NET_H

#define AN_MANUFACTURER   0x6d73
#define AN_PRODUCT        0x6b
#define AN_MAGIC          0x4136384eu      /* 'A68N' */
#define AN_VERSION        1u
#define AN_BOARD_BYTES    0x20000u         /* 128 KB */
#define AN_UNITS          2                /* 0 WiFi, 1 Ethernet (kernel driver to come) */

/* layout */
#define AN_OFF_UNIT(u)    (0x0400u + (unsigned)(u) * 0x0800u)   /* struct an_unit, 2 KB each */
#define AN_OFF_FW         0x1400u          /* struct an_fw[AN_FW_MAX]: firmware offered to the kernel */
#define AN_FW_MAX         8
#define AN_OFF_SCAN       0x1800u          /* struct an_scan[AN_SCAN_MAX]: WiFi scan results */
#define AN_SCAN_MAX       36               /* 36 * 56 bytes: up to 0x1FE0 */
#define AN_OFF_LOG        0x2000u          /* the kernel network log ring (text) */
#define AN_LOG_BYTES      0x2000u
#define AN_SLOT_BYTES     0x0800u          /* one frame slot: struct an_slot */
#define AN_RX_SLOTS       16               /* per unit */
#define AN_TX_SLOTS       12
#define AN_OFF_RX(u)      (0x04000u + (unsigned)(u) * AN_RX_SLOTS * AN_SLOT_BYTES)    /* 0x04000, 0x0C000 */
#define AN_OFF_TX(u)      (0x14000u + (unsigned)(u) * AN_TX_SLOTS * AN_SLOT_BYTES)    /* 0x14000, 0x1A000 */

struct an_hdr {
    unsigned int magic, version, nunits;
    unsigned int irq;               /* kernel: 1 with each new frame, command done or state change; the Amiga clears it */
    unsigned int kernel_ver;        /* the kernel's alice_net version */
    unsigned int amiga_ver;         /* alice68net.device's version << 16 | revision, once it has opened */
    unsigned int log_wr;            /* bytes written to the log ring, ever */
    unsigned int fw_state;          /* AN_FW_*: firmware hand-over (alice68hdf.device at boot) */
    unsigned int fw_count;          /* entries in the AN_OFF_FW table */
    unsigned int boot_us;           /* kernel: microseconds since the Pi started, at the last update */
    unsigned int trace;             /* kernel: 1 with net.trace=1 - the drivers log ICMP frames too */
    unsigned int pad[5];
};                                  /* 64 bytes */

#define AN_FW_NONE      0u
#define AN_FW_OFFERED   1u          /* the Amiga filled the table: the kernel may copy */
#define AN_FW_TAKEN     2u          /* the kernel has its own copies: the Amiga may free its buffers */
#define AN_FW_REFUSED   3u          /* the kernel will not take them (WiFi off, no room) */

struct an_fw {
    char         name[64];          /* file name, e.g. "cyfmac43455-sdio.bin" */
    unsigned int addr;              /* 68k address of the bytes */
    unsigned int len;
};                                  /* 72 bytes */

/* unit kinds and states */
#define AN_KIND_NONE    0u
#define AN_KIND_WIFI    1u
#define AN_KIND_ETHER   2u

#define AN_ST_OFF       0u          /* the same numbers as wifi_api.h's WS_* */
#define AN_ST_STARTING  1u
#define AN_ST_IDLE      2u
#define AN_ST_SCANNING  3u
#define AN_ST_JOINING   4u
#define AN_ST_CONNECTED 5u
#define AN_ST_RETRY     6u
#define AN_ST_FAILED    7u
#define AN_ST_NOFW      8u          /* waiting for firmware */

/* commands (an_unit.cmd); one at a time: the Amiga waits for cmd_done == cmd_seq */
#define AN_CMD_NONE       0u
#define AN_CMD_START      1u        /* bring the hardware up */
#define AN_CMD_JOIN       2u        /* arg: struct an_join */
#define AN_CMD_SCAN       3u        /* results in AN_OFF_SCAN, scan_count */
#define AN_CMD_DISCONNECT 4u
#define AN_CMD_OFF        5u        /* hardware off */
#define AN_CMD_RXON       6u        /* the stack takes frames from now on */
#define AN_CMD_RXOFF      7u
#define AN_CMD_FILTER     8u        /* arg[0..3]: AN_FILT_* */
#define AN_FILT_PROMISC   1u
#define AN_FILT_ALLMULTI  2u
#define AN_CMD_SETMAC     9u        /* arg[0..5]: the unit's MAC address; all zero: the factory one;
                                       arg[6] = 1: only arg[0..2], the OUI, with the factory's last 3 bytes */
#define AN_OUI_COMMODORE  0x008010u /* Commodore International's OUI: both units use it unless net.oui=off */
#define AN_CMD_ETHCFG    10u        /* unit 1. arg[0..3] cfg_adv (AN_LM_*), arg[4..7] cfg_flags (AN_EC_*),
                                       arg[8..11] cfg_speed (10, 100, 1000: without autonegotiation) */
#define AN_CMD_WIFICFG   11u        /* unit 0. arg[4..7] cfg_flags (AN_WC_*) */
#define AN_EC_AUTONEG     0x01u     /* Ethernet: autonegotiate (else cfg_speed and AN_EC_FULL, fixed) */
#define AN_EC_FULL        0x02u     /* fixed full duplex */
#define AN_EC_PAUSE       0x04u     /* flow control: advertise pause and honour it */
#define AN_WC_POWERSAVE   0x01u     /* WiFi: the chip may doze between beacons */

/* an_unit.link_adv / link_lpa: what this side and the other advertise (Ethernet) */
#define AN_LM_10H     0x01u
#define AN_LM_10F     0x02u
#define AN_LM_100H    0x04u
#define AN_LM_100F    0x08u
#define AN_LM_1000H   0x10u
#define AN_LM_1000F   0x20u
#define AN_LM_PAUSE   0x40u
#define AN_LM_ASYM    0x80u
/* an_unit.link_flags */
#define AN_LF_AUTONEG 0x01u         /* autonegotiation done */
#define AN_LF_FULL    0x02u         /* full duplex */
#define AN_LF_RXPAUSE 0x04u         /* flow control: pause frames honoured */
#define AN_LF_TXPAUSE 0x08u

struct an_join {
    char ssid[36];                  /* NUL-terminated, up to 32 bytes */
    char pass[68];                  /* passphrase (8..63) or 64 hex digits; "" for an open network */
    char country[4];                /* "GB", "" for the firmware's default */
};                                  /* 108 bytes */

struct an_unit {
    /* kernel -> Amiga */
    unsigned int kind;              /* AN_KIND_* (0: no such unit) */
    unsigned int state;             /* AN_ST_* */
    unsigned int state_seq;         /* +1 on each change of state or message */
    unsigned int link;              /* 1: frames can flow */
    unsigned int speed_mbps;
    unsigned char mac[8];           /* 6 used */
    char         msg[96];           /* what happened last, for people */
    char         where[96];         /* what the kernel side is doing right now (debugging) */
    char         ssid[36];
    unsigned char bssid[8];
    int          rssi;
    unsigned int channel, security; /* SEC_* (fmac.h) of the network in use */
    unsigned int chip, chiprev, sdmhz;
    char         fwver[96];
    unsigned int rx_pkts, tx_pkts, rx_bytes, tx_bytes, rx_drop, tx_err, sd_errors, polls;
    unsigned int scan_count, scan_seq;
    /* Amiga -> kernel: one command at a time */
    unsigned int cmd_seq;           /* Amiga: +1 after filling cmd and arg */
    unsigned int cmd;
    unsigned int cmd_done;          /* kernel: = cmd_seq when done */
    int          cmd_result;        /* kernel: 0 ok, < 0 error */
    unsigned char arg[128];
    /* frames: rings of slots, indexes counting up for ever (slot = index % slots) */
    unsigned int rx_head;           /* kernel: frames put */
    unsigned int rx_tail;           /* Amiga: frames taken */
    unsigned int tx_head;           /* Amiga: frames put */
    unsigned int tx_tail;           /* kernel: frames sent */
    unsigned int rx_on;             /* kernel's view: the Amiga takes frames */
    unsigned char factory_mac[8];   /* 6 used: the hardware's own (AN_CMD_SETMAC with zeros goes back) */
    unsigned int link_adv, link_lpa;    /* AN_LM_*: advertised here and by the other side (Ethernet) */
    unsigned int link_flags;        /* AN_LF_* */
    unsigned int link_downs;        /* times the link went down since the kernel started */
    unsigned int link_up_s;         /* seconds since the Pi started, when the link last came up */
    unsigned int cfg_adv, cfg_flags, cfg_speed;     /* the settings now (AN_CMD_ETHCFG / AN_CMD_WIFICFG) */
    unsigned int pad[3];
};

struct an_slot {
    unsigned int len;               /* frame bytes */
    unsigned int flags;             /* AN_SF_* */
    unsigned char data[AN_SLOT_BYTES - 8];
};
#define AN_SF_BCAST  1u
#define AN_SF_MCAST  2u

struct an_scan {
    unsigned char bssid[8];
    char         ssid[36];
    int          rssi;
    unsigned int channel, security;
};                                  /* 56 bytes */

#endif
