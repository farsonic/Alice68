/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 kernel Bluetooth: a small Classic (BR/EDR) host for keyboards and
 * mice, host-independent. bt_pi.c gives it the Pi's UART; tools/bt-test runs
 * it on the PC against scripted controller events.
 *
 *   H4 transport -> HCI (commands with the controller's flow control, events,
 *   ACL with fragments) -> Broadcom firmware download (.hcd) -> L2CAP
 *   signalling -> HID host on PSM 0x11/0x13 in boot protocol -> keyboard
 *   reports (8 bytes, as USB) and mouse movements.
 *   Pairing: Secure Simple Pairing (we display a passkey for keyboards, Just
 *   Works for mice) and legacy PIN 0000; link keys kept by the caller.
 *
 *   LE (btle.c, included by bthost.c): HID over GATT - LE connections, SMP
 *   legacy pairing (Just Works for mice, a passkey we show for keyboards; the
 *   controller's LE Encrypt does the AES), bonding with the LTK, GATT
 *   discovery of the HID service, boot protocol (report mode from the Report
 *   Map when a device has no boot reports), reconnects by the controller's
 *   filter accept list (and resolving list, for devices with an IRK).
 *
 * Written from the Bluetooth Core Specification (HCI, L2CAP, SSP, GAP, ATT,
 * GATT, SMP), the HID profile and HID over GATT / HID Service specifications,
 * and Linux's btbcm/hci_bcm for the Broadcom vendor commands. No structs over
 * wire data: every field byte by byte.
 */
#ifndef ALICE68_BTHOST_H
#define ALICE68_BTHOST_H
#include <stdint.h>

#define BT_MAXKEY    6                  /* paired devices */
#define BT_MAXFOUND  12                 /* scan results */
#define BT_MAXCONN   4                  /* links at once */
#define BT_NAME      40

enum { BT_OFF, BT_FW, BT_INIT, BT_READY, BT_FAILED };
#define BTK_CONNECTED 1u
#define BTK_KEYBOARD  2u
#define BTK_MOUSE     4u
#define BTK_LE        8u                /* a Bluetooth LE device: key = its LTK, with ediv and rand */

struct bt_key {
    uint8_t addr[6]; uint8_t key[16]; uint8_t type; uint32_t flags; char name[BT_NAME];
    /* LE: addr is the identity address; addr_type 0 public, 1 random */
    uint8_t addr_type; uint16_t ediv; uint8_t rand[8]; uint8_t irk[16]; uint8_t has_irk;
};
struct bt_found { uint8_t addr[6]; uint32_t cod; int8_t rssi; uint8_t psr; uint16_t clkoff; char name[BT_NAME]; int named; uint8_t le, addr_type; };   /* le: a Bluetooth LE device; addr_type as advertised */

struct bt_io {
    void *ctx;
    void (*send)(void *ctx, const uint8_t *p, unsigned n);        /* bytes to the controller (H4) */
    void (*set_baud)(void *ctx, uint32_t baud);                   /* the UART's speed, once what is queued is out */
    uint32_t (*ms)(void *ctx);                                    /* a millisecond clock */
    void (*log)(void *ctx, const char *line);                     /* one line, no newline */
    void (*keyboard)(void *ctx, const uint8_t rep[8]);            /* a boot keyboard report */
    void (*mouse)(void *ctx, int dx, int dy, int wheel, unsigned buttons);
    void (*keys)(void *ctx);                                      /* the paired list changed: save it */
    void (*kick)(void *ctx, int attempt);                         /* the reset after the firmware went unanswered: unstick the line (may be 0) */
};

struct bt_l2ch { uint16_t psm, lcid, rcid; uint8_t state, conf; };
/* an LE link: pairing (SMP) and the HID service as GATT found it */
#define BT_LE_CHARS 16
#define BT_LE_REPORTS 8
struct bt_kbmap { uint8_t id, ok; uint16_t mod_pos, keys_pos; uint8_t nkeys; };   /* a report-mode keyboard report */
struct bt_le {
    uint8_t addr_type, rat, ra[6];       /* identity type; the address (and type) the link was made with, for c1 */
    uint8_t rekey;                       /* encrypting with the stored LTK */
    /* SMP */
    uint8_t smp, mitm, passkey_method, rdist, got;
    uint8_t preq[7], pres[7], tk[16], mrand[16], srand[16], sconf[16], conf[16], ltk[16], rand[8], irk[16], idaddr[7];
    uint16_t ediv; uint8_t have_sconf, have_srand, keysize;
    uint32_t smp_until;
    /* GATT */
    uint8_t g, gi, att_busy;
    uint16_t svc_s, svc_e, cursor, mtu;
    uint16_t chr_vh[BT_LE_CHARS], chr_uuid[BT_LE_CHARS], chr_cccd[BT_LE_CHARS], chr_ref[BT_LE_CHARS]; uint8_t nchr;
    uint8_t rep_id[BT_LE_CHARS], rep_type[BT_LE_CHARS];
    uint16_t pm, kb_in, ms_in, map;      /* value handles (0: none) */
    uint8_t boot;                        /* boot protocol in use */
    uint8_t mouse_ok; uint8_t hmouse[40];   /* struct hid_mouse (btle.c), opaque here */
    struct bt_kbmap kbm;
    uint8_t lastbtn;
};
struct bt_conn {
    int used;
    uint16_t handle;
    uint8_t addr[6];
    int out;                            /* we connected (and open the channels) */
    int authed, encrypted, pairing, hid_up;
    uint8_t remote_io;                  /* from IO Capability Response, 0xff unknown */
    uint8_t sig_id;
    struct bt_l2ch ch[3];               /* HID control, HID interrupt, SDP */
    uint8_t acl[700];                   /* an L2CAP frame being put together */
    unsigned acl_n, acl_want;
    uint8_t lastkb[8];
    int le;                             /* an LE link (fixed channels: ATT 4, SMP 6, LE signalling 5) */
    struct bt_le l;
};

#define BT_CMDQ 24
#define BT_ACLQ 8
struct bt_host {
    const struct bt_io *io;
    int state;
    char why[64];                       /* BT_FAILED: why */
    uint8_t bdaddr[6];
    /* the controller */
    const uint8_t *fw; unsigned fwlen, fwpos, fwrec;
    int step;                           /* where the start-up is */
    uint32_t wait_until;
    int credits;                        /* HCI commands the controller takes now */
    uint16_t pending_op;                /* the command whose answer we wait for in the start-up, 0 none */
    uint8_t cmdq[BT_CMDQ][260]; uint16_t cmdq_len[BT_CMDQ]; unsigned cq_head, cq_tail;
    uint16_t acl_mtu, acl_free;
    uint8_t aclq[BT_ACLQ][260]; uint16_t aclq_len[BT_ACLQ]; uint8_t aclq_le[BT_ACLQ]; unsigned aq_head, aq_tail;
    uint16_t le_mtu, le_free;           /* the controller's LE buffers (0: LE shares acl_free) */
    /* the H4 receiver */
    uint8_t rx[1100]; unsigned rx_n, rx_need; int rx_type;
    /* devices */
    struct bt_key keys[BT_MAXKEY]; int nkeys; uint32_t keys_gen;
    struct bt_found found[BT_MAXFOUND]; int nfound;
    int scanning; uint32_t scan_end;
    int names_pending;
    struct bt_conn conn[BT_MAXCONN];
    uint8_t pair_addr[6]; int pairing;  /* a pairing the user asked for */
    uint32_t passkey; int passkey_shown;
    uint16_t next_cid;
    char status[64];                    /* in a few words, for the splash and the tool */
    uint32_t reconnect_at; int reconnect_next;
    uint8_t call_addr[6]; int calling;  /* a Create Connection of ours is out */
    int tries;                          /* resets sent after the firmware's launch */
    int le_scanning;                    /* an LE scan beside the inquiry: LE devices are named, not served */
    unsigned heard_other, heard_logged; /* devices a scan heard that are no keyboard or mouse */
    unsigned scan_ir, scan_eir, scan_le; /* inquiry results, extended ones, LE reports in this scan (all, before any filter) */
    /* LE */
    int le_init;                        /* an LE Create Connection is out: 1 to one device, 2 the accept list */
    uint32_t le_init_until;             /* 1: give up then; 0: not again before then */
    int le_cancelling;                  /* an LE Create Connection Cancel is out */
    int le_lists_dirty;                 /* the accept/resolving lists must be written again */
    int aes_busy; uint16_t aes_handle;  /* an LE Encrypt / LE Rand of SMP's is out, for that link */
    uint8_t rmap[512]; unsigned rmap_n; /* a Report Map being read (one link at a time) */
};

/* fw/fwlen: the .hcd file (0: none - the controller's ROM firmware is used) */
void bt_init(struct bt_host *h, const struct bt_io *io, const uint8_t *fw, unsigned fwlen);
void bt_rx(struct bt_host *h, const uint8_t *p, unsigned n);       /* bytes from the controller */
void bt_poll(struct bt_host *h);                                   /* timers; call often */
/* a paired device known from before (cmdline.txt), before or after bt_init */
int  bt_add_key(struct bt_host *h, const uint8_t addr[6], const uint8_t key[16], uint8_t type, uint32_t kind);
/* an LE one: identity address and type, LTK, EDIV, Rand, IRK (0: none) */
int  bt_add_le_key(struct bt_host *h, const uint8_t addr[6], uint8_t addr_type, const uint8_t ltk[16], uint16_t ediv,
                   const uint8_t rand[8], const uint8_t *irk, uint32_t kind);
int  bt_scan(struct bt_host *h);                                   /* ~10 s inquiry: found[] fills */
int  bt_pair(struct bt_host *h, const uint8_t addr[6]);
int  bt_connect(struct bt_host *h, const uint8_t addr[6]);         /* a paired device, now */
int  bt_forget(struct bt_host *h, const uint8_t addr[6]);
/* "AA:BB:CC:DD:EE:FF" (addr as the controller gives it: least significant byte first) */
void bt_addr_str(const uint8_t addr[6], char out[18]);
#endif
