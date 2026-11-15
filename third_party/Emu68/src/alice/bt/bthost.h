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
 * Written from the Bluetooth Core Specification (HCI, L2CAP, SSP), the HID
 * profile specification, and Linux's btbcm/hci_bcm for the Broadcom vendor
 * commands. No structs over wire data: every field byte by byte.
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

struct bt_key { uint8_t addr[6]; uint8_t key[16]; uint8_t type; uint32_t flags; char name[BT_NAME]; };
struct bt_found { uint8_t addr[6]; uint32_t cod; int8_t rssi; uint8_t psr; uint16_t clkoff; char name[BT_NAME]; int named; uint8_t le; };   /* le: a Bluetooth LE device (not served yet) */

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
    uint8_t aclq[BT_ACLQ][260]; uint16_t aclq_len[BT_ACLQ]; unsigned aq_head, aq_tail;
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
};

/* fw/fwlen: the .hcd file (0: none - the controller's ROM firmware is used) */
void bt_init(struct bt_host *h, const struct bt_io *io, const uint8_t *fw, unsigned fwlen);
void bt_rx(struct bt_host *h, const uint8_t *p, unsigned n);       /* bytes from the controller */
void bt_poll(struct bt_host *h);                                   /* timers; call often */
/* a paired device known from before (cmdline.txt), before or after bt_init */
int  bt_add_key(struct bt_host *h, const uint8_t addr[6], const uint8_t key[16], uint8_t type, uint32_t kind);
int  bt_scan(struct bt_host *h);                                   /* ~10 s inquiry: found[] fills */
int  bt_pair(struct bt_host *h, const uint8_t addr[6]);
int  bt_connect(struct bt_host *h, const uint8_t addr[6]);         /* a paired device, now */
int  bt_forget(struct bt_host *h, const uint8_t addr[6]);
/* "AA:BB:CC:DD:EE:FF" (addr as the controller gives it: least significant byte first) */
void bt_addr_str(const uint8_t addr[6], char out[18]);
#endif
