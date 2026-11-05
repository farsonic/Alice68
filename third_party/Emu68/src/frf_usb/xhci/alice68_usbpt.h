/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 USB pass-through mailbox - shared by the kernel (frf_xhci.c, ARM,
 * big-endian) and alice68-usb.device (68k, big-endian). Same byte order on
 * both sides, every field 32-bit aligned: no swapping, no packing pragmas.
 *
 * The kernel drives the Pi 4's VL805 itself (keyboards and mice to the
 * virtual Amiga). Every other device it finds is offered here: as a port of
 * a root hub that alice68-usb.device emulates for Neptune, with transfers
 * carried out by the kernel on its own xHCI.
 *
 * Lives in a 64 KB Zorro II I/O board, manufacturer 0x6d73, product 0x6a,
 * uncached on both sides.
 */
#ifndef ALICE68_USBPT_H
#define ALICE68_USBPT_H

#define PT_MANUFACTURER   0x6d73
#define PT_PRODUCT        0x6a
#define PT_MAGIC          0x45415550u      /* 'EAUP' */
#define PT_VERSION        2u
#define PT_BOARD_BYTES    0x10000u
#define PT_PORTS          8
#define PT_REQS           32
#define PT_MAXCHUNK       0x8000u          /* kernel bounce buffer per request */

#define PT_OFF_REQS       0x0200u          /* PT_REQS * 64 bytes */
#define PT_OFF_ALOG       0x1000u          /* the Amiga driver's log ring */
#define PT_ALOG_BYTES     0x3000u
#define PT_OFF_KLOG       0x8000u          /* the kernel's [FRF-XHCI] log ring */
#define PT_KLOG_BYTES     0x8000u

/* request states (pt_req.state) */
#define PTS_FREE    0u      /* the Amiga may take it */
#define PTS_SUBMIT  1u      /* filled in by the Amiga, for the kernel */
#define PTS_BUSY    2u      /* the kernel is on it */
#define PTS_DONE    3u      /* the kernel finished: actual, error valid */

/* request types */
#define PTT_CONTROL 0u
#define PTT_BULK    1u
#define PTT_INT     2u

/* directions, as Neptune's iouh_Dir */
#define PTD_SETUP   0u
#define PTD_OUT     1u
#define PTD_IN      2u

struct pt_port {
    unsigned int present;       /* 1: a device the Amiga may have */
    unsigned int speed;         /* xHCI speed: 1 full, 2 low, 3 high, 4 super */
    unsigned int gen;           /* bumped on every attach/detach */
    unsigned int vidpid;
};

struct pt_req {
    unsigned int  state;
    unsigned int  port;         /* 1..PT_PORTS */
    unsigned int  type;         /* PTT_* */
    unsigned int  ep;           /* endpoint number 0..15 */
    unsigned int  dir;          /* PTD_* */
    unsigned int  len;
    unsigned int  data;         /* 68k address of the buffer */
    unsigned char setup[8];     /* control: the setup packet, wire order */
    unsigned int  flags;        /* Neptune's iouh_Flags */
    unsigned int  maxpkt;
    unsigned int  actual;       /* kernel: bytes moved */
    int           error;        /* kernel: UHIOERR_* or -2 aborted */
    unsigned int  cancel;       /* Amiga: AbortIO */
    unsigned int  seq;          /* Amiga: submission order (one endpoint's requests run in it) */
    unsigned int  naktimeout;   /* ms, when flags has UHFF_NAKTIMEOUT */
};                              /* 64 bytes */

struct pt_hdr {
    unsigned int magic, version, nports, nreqs;
    unsigned int irq;           /* kernel sets 1 with each completion/port change; the Amiga clears it */
    unsigned int port_gen;      /* bumped on every port change */
    unsigned int klog_wr;       /* bytes written to the kernel log ring, ever */
    unsigned int alog_wr;       /* ...and to the Amiga driver's */
    unsigned int amiga_ver;     /* the driver's version once it has opened */
    unsigned int pad[7];
    struct pt_port port[PT_PORTS];
};                              /* 64 + 128 bytes */

#endif
