/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 kernel Bluetooth: the host - see bthost.h. */
#include <stdarg.h>
#include "bthost.h"

/* ---- small helpers (no C library) -------------------------------------------- */
static void mcpy(void *d, const void *s, unsigned n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mset(void *d, int c, unsigned n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; }
static int  mcmp(const void *a, const void *b, unsigned n) { const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 1; return 0; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t now(struct bt_host *h) { return h->io->ms(h->io->ctx); }
static int due(struct bt_host *h, uint32_t t) { return (int32_t)(now(h) - t) >= 0; }

/* a log line: %s %u %d %x %02x %a (an address) */
static void lg(struct bt_host *h, const char *fmt, ...)
{
    char b[160];
    unsigned n = 0;
    va_list ap;
    va_start(ap, fmt);
#define PUT(c) do { if (n < sizeof b - 1) b[n++] = (char)(c); } while (0)
    for (const char *p = "[BT] "; *p; p++) PUT(*p);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        int w = 0;
        if (*fmt == '0') { fmt++; w = *fmt++ - '0'; }
        if (*fmt == 's') { const char *s = va_arg(ap, const char *); while (s && *s) PUT(*s++); }
        else if (*fmt == 'a') { char a[18]; bt_addr_str(va_arg(ap, const uint8_t *), a); for (int i = 0; a[i]; i++) PUT(a[i]); }
        else if (*fmt == 'u' || *fmt == 'd' || *fmt == 'x') {
            uint32_t v = va_arg(ap, uint32_t);
            char t[12]; int k = 0;
            if (*fmt == 'd' && (int32_t)v < 0) { PUT('-'); v = (uint32_t)-(int32_t)v; }
            unsigned base = *fmt == 'x' ? 16 : 10;
            do { t[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            while (k < w) t[k++] = '0';
            while (k) PUT(t[--k]);
        }
    }
#undef PUT
    va_end(ap);
    b[n] = 0;
    if (h->io->log) h->io->log(h->io->ctx, b);
}
static void say(struct bt_host *h, const char *s) { unsigned i = 0; for (; s[i] && i + 1 < sizeof h->status; i++) h->status[i] = s[i]; h->status[i] = 0; }

void bt_addr_str(const uint8_t a[6], char o[18])
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) { o[i * 3] = hx[a[5 - i] >> 4]; o[i * 3 + 1] = hx[a[5 - i] & 15]; o[i * 3 + 2] = i < 5 ? ':' : 0; }
}

/* ---- HCI commands ------------------------------------------------------------ */
#define OP(ogf, ocf) ((uint16_t)((ogf) << 10 | (ocf)))
#define C_INQUIRY        OP(1, 0x01)
#define C_INQUIRY_CANCEL OP(1, 0x02)
#define C_CREATE_CONN    OP(1, 0x05)
#define C_DISCONNECT     OP(1, 0x06)
#define C_ACCEPT_CONN    OP(1, 0x09)
#define C_REJECT_CONN    OP(1, 0x0a)
#define C_LINKKEY_REPLY  OP(1, 0x0b)
#define C_LINKKEY_NEG    OP(1, 0x0c)
#define C_PIN_REPLY      OP(1, 0x0d)
#define C_AUTH_REQ       OP(1, 0x11)
#define C_SET_ENCRYPT    OP(1, 0x13)
#define C_REMOTE_NAME    OP(1, 0x19)
#define C_IOCAP_REPLY    OP(1, 0x2b)
#define C_CONFIRM_REPLY  OP(1, 0x2c)
#define C_LINK_POLICY    OP(2, 0x0f)
#define C_EVENT_MASK     OP(3, 0x01)
#define C_RESET          OP(3, 0x03)
#define C_LOCAL_NAME     OP(3, 0x13)
#define C_READ_NAME      OP(3, 0x14)
#define C_PAGE_TIMEOUT   OP(3, 0x18)
#define C_SCAN_ENABLE    OP(3, 0x1a)
#define C_CLASS          OP(3, 0x24)
#define C_INQUIRY_MODE   OP(3, 0x45)
#define C_SSP_MODE       OP(3, 0x56)
#define C_LOCAL_VERSION  OP(4, 0x01)
#define C_BUFFER_SIZE    OP(4, 0x05)
#define C_BDADDR         OP(4, 0x09)
#define C_BCM_MINIDRIVER 0xfc2e
#define C_BCM_LAUNCH     0xfc4e

static void cmd_flush(struct bt_host *h)
{
    while (h->credits > 0 && h->cq_head != h->cq_tail) {
        unsigned i = h->cq_head % BT_CMDQ;
        h->io->send(h->io->ctx, h->cmdq[i], h->cmdq_len[i]);
        h->cq_head++;
        h->credits--;
    }
}
static void cmd(struct bt_host *h, uint16_t op, const uint8_t *p, unsigned n)
{
    if (h->cq_tail - h->cq_head >= BT_CMDQ || n > 255) { lg(h, "command %x dropped: queue full", op); return; }
    unsigned i = h->cq_tail % BT_CMDQ;
    uint8_t *b = h->cmdq[i];
    b[0] = 0x01; put16(b + 1, op); b[3] = (uint8_t)n;
    if (n) mcpy(b + 4, p, n);
    h->cmdq_len[i] = (uint16_t)(n + 4);
    h->cq_tail++;
    cmd_flush(h);
}
static void cmd_addr(struct bt_host *h, uint16_t op, const uint8_t a[6]) { cmd(h, op, a, 6); }
static void cmd_handle(struct bt_host *h, uint16_t op, uint16_t handle) { uint8_t p[2]; put16(p, handle); cmd(h, op, p, 2); }

/* ---- ACL out ----------------------------------------------------------------- */
static void acl_flush(struct bt_host *h)
{
    while (h->acl_free > 0 && h->aq_head != h->aq_tail) {
        unsigned i = h->aq_head % BT_ACLQ;
        h->io->send(h->io->ctx, h->aclq[i], h->aclq_len[i]);
        h->aq_head++;
        h->acl_free--;
    }
}
/* an L2CAP frame on cid: one ACL packet (ours are all short) */
static void l2_send(struct bt_host *h, struct bt_conn *c, uint16_t cid, const uint8_t *d, unsigned n)
{
    if (h->aq_tail - h->aq_head >= BT_ACLQ || n + 9 > sizeof h->aclq[0]) { lg(h, "ACL frame dropped"); return; }
    unsigned i = h->aq_tail % BT_ACLQ;
    uint8_t *b = h->aclq[i];
    b[0] = 0x02;
    put16(b + 1, (unsigned)(c->handle & 0x0fff) | 0x2000);      /* first, automatically flushable */
    put16(b + 3, n + 4);
    put16(b + 5, n); put16(b + 7, cid);
    mcpy(b + 9, d, n);
    h->aclq_len[i] = (uint16_t)(n + 9);
    h->aq_tail++;
    acl_flush(h);
}

/* ---- keys and devices ---------------------------------------------------------- */
static struct bt_key *key_of(struct bt_host *h, const uint8_t a[6])
{
    for (int i = 0; i < h->nkeys; i++) if (!mcmp(h->keys[i].addr, a, 6)) return &h->keys[i];
    return 0;
}
static struct bt_found *found_of(struct bt_host *h, const uint8_t a[6])
{
    for (int i = 0; i < h->nfound; i++) if (!mcmp(h->found[i].addr, a, 6)) return &h->found[i];
    return 0;
}
static uint32_t kind_of_cod(uint32_t cod)
{
    if (((cod >> 8) & 0x1f) != 5) return 0;          /* not a peripheral */
    uint32_t m = (cod >> 6) & 3;
    return (m & 1 ? BTK_KEYBOARD : 0) | (m & 2 ? BTK_MOUSE : 0);
}
int bt_add_key(struct bt_host *h, const uint8_t a[6], const uint8_t key[16], uint8_t type, uint32_t kind)
{
    struct bt_key *k = key_of(h, a);
    if (!k) {
        if (h->nkeys >= BT_MAXKEY) return -1;
        k = &h->keys[h->nkeys++];
        mset(k, 0, sizeof *k);
        mcpy(k->addr, a, 6);
    }
    mcpy(k->key, key, 16);
    k->type = type;
    k->flags = (k->flags & BTK_CONNECTED) | (kind & (BTK_KEYBOARD | BTK_MOUSE));
    return 0;
}
static void keys_changed(struct bt_host *h) { h->keys_gen++; if (h->io->keys) h->io->keys(h->io->ctx); }

static struct bt_conn *conn_of(struct bt_host *h, uint16_t handle)
{
    for (int i = 0; i < BT_MAXCONN; i++) if (h->conn[i].used && h->conn[i].handle == handle) return &h->conn[i];
    return 0;
}
static struct bt_conn *conn_by_addr(struct bt_host *h, const uint8_t a[6])
{
    for (int i = 0; i < BT_MAXCONN; i++) if (h->conn[i].used && !mcmp(h->conn[i].addr, a, 6)) return &h->conn[i];
    return 0;
}
static void set_connected(struct bt_host *h, const uint8_t a[6], int on)
{
    struct bt_key *k = key_of(h, a);
    if (k) k->flags = on ? k->flags | BTK_CONNECTED : k->flags & ~BTK_CONNECTED;
}

/* ---- the start-up --------------------------------------------------------------- */
enum { S_RESET, S_MINIDRIVER, S_MINI_WAIT, S_PATCH, S_LAUNCH_WAIT, S_RESET2, S_SETUP, S_DONE, S_RESET_PAUSE, S_RESET2_PAUSE, S_ROM_VER, S_ROM_NAME };
#define RESET_PAUSE  100                    /* ms after a reset's answer before the next command (Linux btbcm_reset) */
#define RESET2_TRIES 5                      /* resets after the launch before giving up */
static const uint8_t ev_mask[8] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };   /* as Linux for 2.1+: SSP events in */

static void fail(struct bt_host *h, const char *why)
{
    h->state = BT_FAILED;
    unsigned i = 0;
    for (; why[i] && i + 1 < sizeof h->why; i++) h->why[i] = why[i];
    h->why[i] = 0;
    say(h, why);
    lg(h, "FAIL: %s", why);
}
static void sync_cmd(struct bt_host *h, uint16_t op, const uint8_t *p, unsigned n)
{
    h->pending_op = op;
    h->wait_until = now(h) + 3000;
    cmd(h, op, p, n);
}
/* the next set-up command after the controller is running its firmware */
static void setup_next(struct bt_host *h)
{
    static const char name[] = "Alice68";
    uint8_t p[248];
    switch (h->step++) {
    case 0: sync_cmd(h, C_LOCAL_VERSION, 0, 0); break;
    case 1: sync_cmd(h, C_BDADDR, 0, 0); break;
    case 2: sync_cmd(h, C_BUFFER_SIZE, 0, 0); break;
    case 3: sync_cmd(h, C_EVENT_MASK, ev_mask, 8); break;
    case 4: p[0] = 1; sync_cmd(h, C_SSP_MODE, p, 1); break;
    case 5: p[0] = 2; sync_cmd(h, C_INQUIRY_MODE, p, 1); break;                 /* results with RSSI and names */
    case 6: p[0] = 0x04; p[1] = 0x01; p[2] = 0x00; sync_cmd(h, C_CLASS, p, 3); break;  /* computer, desktop */
    case 7: mset(p, 0, 248); mcpy(p, name, sizeof name); sync_cmd(h, C_LOCAL_NAME, p, 248); break;
    case 8: put16(p, 0x2000); sync_cmd(h, C_PAGE_TIMEOUT, p, 2); break;          /* 5.12 s */
    case 9: put16(p, 0x0005); sync_cmd(h, C_LINK_POLICY, p, 2); break;           /* role switch, sniff */
    case 10: p[0] = 0x02; sync_cmd(h, C_SCAN_ENABLE, p, 1); break;               /* page scan: paired devices reconnect */
    default:
        h->pending_op = 0;
        h->state = BT_READY;
        say(h, h->nkeys ? "ready" : "ready - nothing paired yet");
        lg(h, "ready: %a, %u paired", h->bdaddr, (uint32_t)h->nkeys);
        h->reconnect_at = now(h) + 300;
        h->reconnect_next = 0;
        break;
    }
}
static void patch_next(struct bt_host *h)
{
    if (h->fwpos + 3 > h->fwlen) {                   /* all sent (the last one launches it) */
        h->step = S_LAUNCH_WAIT;
        h->wait_until = now(h) + 250;
        h->pending_op = 0;
        lg(h, "firmware: %u records sent, starting it", h->fwrec);
        return;
    }
    const uint8_t *r = h->fw + h->fwpos;
    uint16_t op = le16(r);
    unsigned n = r[2];
    if (h->fwpos + 3 + n > h->fwlen) { fail(h, "the firmware file is cut short"); return; }
    h->fwpos += 3 + n;
    h->fwrec++;
    sync_cmd(h, op, r + 3, n);
}
/* the firmware file: whole records, the last one Launch_RAM - else it is cut short or not an .hcd */
static int fw_check(struct bt_host *h)
{
    unsigned pos = 0, recs = 0; uint16_t last = 0;
    while (pos + 3 <= h->fwlen) { last = le16(h->fw + pos); pos += 3u + h->fw[pos + 2]; recs++; }
    lg(h, "firmware file: %u bytes, %u records, the last %x", h->fwlen, recs, (uint32_t)last);
    if (pos != h->fwlen || last != C_BCM_LAUNCH) { lg(h, "the firmware file is not whole (it must end with Launch_RAM fc4e): not loaded"); return 0; }
    return 1;
}
static void after_rom(struct bt_host *h)
{
    h->step = S_MINIDRIVER;
    sync_cmd(h, C_BCM_MINIDRIVER, 0, 0);
}
/* a Command Complete for the start-up's command */
static void startup_answer(struct bt_host *h, uint16_t op, const uint8_t *ret, unsigned n)
{
    uint8_t st = n ? ret[0] : 0xff;
    if (op != h->pending_op) return;
    h->pending_op = 0;
    if (h->state == BT_FW) switch (h->step) {
    case S_ROM_VER:                              /* the chip as its ROM says: which firmware it wants */
        if (n >= 9) lg(h, "chip ROM: HCI %u, revision %x, LMP subversion %x, manufacturer %u", ret[1], le16(ret + 2), le16(ret + 7), le16(ret + 5));
        h->step = S_ROM_NAME; sync_cmd(h, C_READ_NAME, 0, 0); h->wait_until = now(h) + 1000;
        return;
    case S_ROM_NAME: {
        char nm[33]; unsigned i = 0;
        for (; i < 32 && i + 1 < n && ret[1 + i] >= 32 && ret[1 + i] < 127; i++) nm[i] = (char)ret[1 + i];
        nm[i] = 0;
        lg(h, "chip ROM name: %s (it wants %s.hcd)", i ? nm : "(none)", i ? nm : "?");
        after_rom(h);
        return;
    }
    case S_RESET:
        if (st) { fail(h, "the chip refused a reset"); return; }
        h->step = S_RESET_PAUSE;                 /* the chip is still resetting: a command now can be lost */
        h->wait_until = now(h) + RESET_PAUSE;
        return;
    case S_MINIDRIVER:
        if (st) { fail(h, "the chip would not take its firmware"); return; }
        h->step = S_MINI_WAIT;
        h->wait_until = now(h) + 50;
        return;
    case S_PATCH:
        if (st) { lg(h, "firmware record %x refused (%u)", op, st); fail(h, "the chip refused its firmware"); return; }
        patch_next(h);
        return;
    case S_RESET2:
        if (st) { fail(h, "the chip refused a reset after its firmware"); return; }
        if (h->tries > 1) lg(h, "the new firmware answered the reset (try %u)", (uint32_t)h->tries);
        h->step = S_RESET2_PAUSE;
        h->wait_until = now(h) + RESET_PAUSE;
        return;
    }
    /* the set-up after the firmware (step counts on) */
    if (h->state == BT_INIT) {
        if (op == C_BDADDR && n >= 7) mcpy(h->bdaddr, ret + 1, 6);
        if (op == C_BUFFER_SIZE && n >= 8) { h->acl_mtu = le16(ret + 1); h->acl_free = le16(ret + 4); if (!h->acl_free) h->acl_free = 1; }
        if (op == C_LOCAL_VERSION && n >= 9) lg(h, "controller: HCI %u, revision %x, LMP %x, manufacturer %u", ret[1], le16(ret + 2), le16(ret + 6), le16(ret + 4));
        if (st && (op == C_BDADDR || op == C_BUFFER_SIZE)) { fail(h, "the chip did not answer its set-up"); return; }
        if (st) lg(h, "set-up command %x: status %u (going on)", op, st);
        setup_next(h);
    }
}

void bt_init(struct bt_host *h, const struct bt_io *io, const uint8_t *fw, unsigned fwlen)
{
    struct bt_key keep[BT_MAXKEY];
    int nk = h->nkeys;
    mcpy(keep, h->keys, sizeof keep);
    mset(h, 0, sizeof *h);
    mcpy(h->keys, keep, sizeof keep);
    h->nkeys = nk;
    for (int i = 0; i < nk; i++) h->keys[i].flags &= ~BTK_CONNECTED;
    h->io = io;
    h->fw = fw; h->fwlen = fwlen;
    if (fw && !fw_check(h)) { h->fw = 0; h->fwlen = 0; }
    h->credits = 1;
    h->acl_free = 1;
    h->next_cid = 0x40;
    h->state = BT_FW;
    say(h, h->fw ? "loading its firmware" : "starting");
    h->step = S_RESET;
    sync_cmd(h, C_RESET, 0, 0);
}

/* ---- L2CAP -------------------------------------------------------------------- */
#define PSM_SDP     0x0001
#define PSM_HIDCTL  0x0011
#define PSM_HIDINT  0x0013
enum { CH_FREE, CH_WAIT_CONN, CH_CONFIG, CH_OPEN };
#define CONF_OURS   1                       /* our request answered */
#define CONF_THEIRS 2                       /* theirs answered */

static void sig(struct bt_host *h, struct bt_conn *c, uint8_t code, uint8_t id, const uint8_t *d, unsigned n)
{
    uint8_t b[64];
    if (n > sizeof b - 4) return;
    b[0] = code; b[1] = id; put16(b + 2, n);
    mcpy(b + 4, d, n);
    l2_send(h, c, 0x0001, b, n + 4);
}
static struct bt_l2ch *ch_by_lcid(struct bt_conn *c, uint16_t lcid)
{
    for (int i = 0; i < 3; i++) if (c->ch[i].state != CH_FREE && c->ch[i].lcid == lcid) return &c->ch[i];
    return 0;
}
static int ch_slot(uint16_t psm) { return psm == PSM_HIDCTL ? 0 : psm == PSM_HIDINT ? 1 : 2; }
static void conf_req(struct bt_host *h, struct bt_conn *c, struct bt_l2ch *ch)
{
    uint8_t d[8];
    put16(d, ch->rcid); put16(d + 2, 0);
    d[4] = 0x01; d[5] = 2; put16(d + 6, 672);           /* MTU 672 */
    sig(h, c, 0x04, ++c->sig_id, d, 8);
}
static void ch_open_out(struct bt_host *h, struct bt_conn *c, uint16_t psm)
{
    struct bt_l2ch *ch = &c->ch[ch_slot(psm)];
    mset(ch, 0, sizeof *ch);
    ch->psm = psm;
    ch->lcid = h->next_cid++;
    ch->state = CH_WAIT_CONN;
    uint8_t d[4];
    put16(d, psm); put16(d + 2, ch->lcid);
    sig(h, c, 0x02, ++c->sig_id, d, 4);
}
static void hid_check(struct bt_host *h, struct bt_conn *c)
{
    if (c->hid_up || c->ch[0].state != CH_OPEN || c->ch[1].state != CH_OPEN) return;
    c->hid_up = 1;
    uint8_t p = 0x70;                                   /* SET_PROTOCOL: boot */
    l2_send(h, c, c->ch[0].rcid, &p, 1);
    set_connected(h, c->addr, 1);
    struct bt_key *k = key_of(h, c->addr);
    lg(h, "%a connected (HID, boot protocol)", c->addr);
    say(h, k && (k->flags & BTK_KEYBOARD) ? "keyboard connected" : k && (k->flags & BTK_MOUSE) ? "mouse connected" : "device connected");
}
static void ch_opened(struct bt_host *h, struct bt_conn *c, struct bt_l2ch *ch)
{
    if ((ch->conf & (CONF_OURS | CONF_THEIRS)) != (CONF_OURS | CONF_THEIRS)) return;
    ch->state = CH_OPEN;
    if (ch->psm == PSM_HIDCTL && c->out && c->ch[1].state == CH_FREE) ch_open_out(h, c, PSM_HIDINT);
    hid_check(h, c);
}

/* SDP: we offer no services - an empty answer to any question */
static void sdp_in(struct bt_host *h, struct bt_conn *c, struct bt_l2ch *ch, const uint8_t *p, unsigned n)
{
    if (n < 5) return;
    uint8_t r[16];
    unsigned k;
    r[1] = p[1]; r[2] = p[2];
    if (p[0] == 0x02) { r[0] = 0x03; r[3] = 0; r[4] = 5; mset(r + 5, 0, 5); k = 10; }
    else if (p[0] == 0x04 || p[0] == 0x06) { r[0] = (uint8_t)(p[0] + 1); r[3] = 0; r[4] = 5; r[5] = 0; r[6] = 2; r[7] = 0x35; r[8] = 0; r[9] = 0; k = 10; }
    else { r[0] = 0x01; r[3] = 0; r[4] = 2; r[5] = 0; r[6] = 3; k = 7; }
    l2_send(h, c, ch->rcid, r, k);
}

static void hid_in(struct bt_host *h, struct bt_conn *c, const uint8_t *p, unsigned n)
{
    if (n < 2 || p[0] != 0xa1) return;                  /* DATA | Input */
    struct bt_key *k = key_of(h, c->addr);
    if (p[1] == 0x01 && n >= 10) {                      /* boot keyboard */
        if (k) k->flags |= BTK_KEYBOARD;
        mcpy(c->lastkb, p + 2, 8);
        if (h->io->keyboard) h->io->keyboard(h->io->ctx, p + 2);
    } else if (p[1] == 0x02 && n >= 5) {                /* boot mouse (+ a wheel byte some send) */
        if (k) k->flags |= BTK_MOUSE;
        if (h->io->mouse) h->io->mouse(h->io->ctx, (int8_t)p[3], (int8_t)p[4], n >= 6 ? (int8_t)p[5] : 0, p[2]);
    }
}

static void sig_in(struct bt_host *h, struct bt_conn *c, const uint8_t *p, unsigned n)
{
    while (n >= 4) {
        uint8_t code = p[0], id = p[1];
        unsigned len = le16(p + 2);
        if (len + 4 > n) return;
        const uint8_t *d = p + 4;
        uint8_t r[16];
        switch (code) {
        case 0x02: {                                    /* Connection Request */
            if (len < 4) break;
            uint16_t psm = le16(d), scid = le16(d + 2);
            int ok = psm == PSM_HIDCTL || psm == PSM_HIDINT || psm == PSM_SDP;
            struct bt_l2ch *ch = ok ? &c->ch[ch_slot(psm)] : 0;
            if (ch) {
                mset(ch, 0, sizeof *ch);
                ch->psm = psm; ch->lcid = h->next_cid++; ch->rcid = scid; ch->state = CH_CONFIG;
            }
            put16(r, ch ? ch->lcid : 0); put16(r + 2, scid); put16(r + 4, ch ? 0 : 2); put16(r + 6, 0);
            sig(h, c, 0x03, id, r, 8);
            if (ch) conf_req(h, c, ch);
            break;
        }
        case 0x03: {                                    /* Connection Response */
            if (len < 8) break;
            uint16_t dcid = le16(d), scid = le16(d + 2), res = le16(d + 4);
            struct bt_l2ch *ch = ch_by_lcid(c, scid);
            if (!ch || ch->state != CH_WAIT_CONN) break;
            if (res == 1) break;                        /* pending */
            if (res) { lg(h, "%a refused HID channel %x (%u)", c->addr, ch->psm, res); ch->state = CH_FREE; break; }
            ch->rcid = dcid; ch->state = CH_CONFIG;
            conf_req(h, c, ch);
            break;
        }
        case 0x04: {                                    /* Configure Request: we take any options */
            if (len < 4) break;
            struct bt_l2ch *ch = ch_by_lcid(c, le16(d));
            if (!ch) { put16(r, 2); put16(r + 2, le16(d)); put16(r + 4, 0); sig(h, c, 0x01, id, r, 6); break; }
            put16(r, ch->rcid); put16(r + 2, 0); put16(r + 4, 0);
            sig(h, c, 0x05, id, r, 6);
            if (le16(d + 2) & 1) break;                 /* more to come */
            ch->conf |= CONF_THEIRS;
            ch_opened(h, c, ch);
            break;
        }
        case 0x05: {                                    /* Configure Response */
            if (len < 6) break;
            struct bt_l2ch *ch = ch_by_lcid(c, le16(d));
            if (!ch) break;
            if (le16(d + 4)) { lg(h, "%a: channel configuration refused (%u)", c->addr, le16(d + 4)); break; }
            ch->conf |= CONF_OURS;
            ch_opened(h, c, ch);
            break;
        }
        case 0x06: {                                    /* Disconnection Request */
            if (len < 4) break;
            struct bt_l2ch *ch = ch_by_lcid(c, le16(d));
            mcpy(r, d, 4);
            sig(h, c, 0x07, id, r, 4);
            if (ch) { ch->state = CH_FREE; if (ch->psm != PSM_SDP) c->hid_up = 0; }
            break;
        }
        case 0x07: break;                               /* Disconnection Response */
        case 0x08: sig(h, c, 0x09, id, d, len > 44 ? 44 : len); break;   /* Echo */
        case 0x0a: {                                    /* Information Request */
            uint16_t t = len >= 2 ? le16(d) : 0;
            put16(r, t);
            if (t == 2) { put16(r + 2, 0); mset(r + 4, 0, 4); sig(h, c, 0x0b, id, r, 8); }
            else if (t == 3) { put16(r + 2, 0); mset(r + 4, 0, 8); r[4] = 0x02; sig(h, c, 0x0b, id, r, 12); }
            else { put16(r + 2, 1); sig(h, c, 0x0b, id, r, 4); }
            break;
        }
        case 0x01: case 0x0b: break;                    /* Command Reject, Information Response */
        default: put16(r, 0); sig(h, c, 0x01, id, r, 2); break;
        }
        p += 4 + len; n -= 4 + len;
    }
}

static void l2_in(struct bt_host *h, struct bt_conn *c, const uint8_t *f, unsigned n)
{
    if (n < 4) return;
    unsigned len = le16(f);
    uint16_t cid = le16(f + 2);
    if (len + 4 > n) return;
    if (cid == 0x0001) { sig_in(h, c, f + 4, len); return; }
    struct bt_l2ch *ch = ch_by_lcid(c, cid);
    if (!ch || ch->state != CH_OPEN) return;
    if (ch->psm == PSM_HIDINT) hid_in(h, c, f + 4, len);
    else if (ch->psm == PSM_HIDCTL) { if (len && f[4] == 0x15) lg(h, "%a unplugged itself", c->addr); }   /* VIRTUAL_CABLE_UNPLUG */
    else if (ch->psm == PSM_SDP) sdp_in(h, c, ch, f + 4, len);
}

static void acl_in(struct bt_host *h, const uint8_t *p, unsigned n)
{
    if (n < 4) return;
    uint16_t hf = le16(p), len = le16(p + 2);
    struct bt_conn *c = conn_of(h, hf & 0x0fff);
    if (!c || (unsigned)len + 4 > n) return;
    unsigned pb = (hf >> 12) & 3;
    const uint8_t *d = p + 4;
    if (pb == 2 || pb == 0) {                           /* a frame starts */
        if (len >= 4 && (unsigned)le16(d) + 4 <= len) { l2_in(h, c, d, len); c->acl_n = 0; return; }
        if (len > sizeof c->acl || len < 2) { c->acl_n = 0; return; }
        mcpy(c->acl, d, len); c->acl_n = len; c->acl_want = (unsigned)le16(d) + 4;
        if (c->acl_want > sizeof c->acl) c->acl_n = 0;
    } else if (pb == 1 && c->acl_n) {                   /* continues */
        if (c->acl_n + len > sizeof c->acl) { c->acl_n = 0; return; }
        mcpy(c->acl + c->acl_n, d, len); c->acl_n += len;
        if (c->acl_n >= c->acl_want) { l2_in(h, c, c->acl, c->acl_n); c->acl_n = 0; }
    }
}

/* ---- events ------------------------------------------------------------------- */
static void create_conn(struct bt_host *h, const uint8_t a[6])
{
    struct bt_found *f = found_of(h, a);
    uint8_t p[13];
    mcpy(p, a, 6);
    put16(p + 6, 0xcc18);                               /* DM1 DH1 DM3 DH3 DM5 DH5 */
    p[8] = f ? f->psr : 0x01;
    p[9] = 0;
    put16(p + 10, f ? (unsigned)(f->clkoff | 0x8000) : 0);
    p[12] = 0x01;                                       /* role switch allowed */
    mcpy(h->call_addr, a, 6);
    h->calling = 1;
    cmd(h, C_CREATE_CONN, p, 13);
}
static void open_conn_done(struct bt_host *h, struct bt_conn *c)
{
    if (c->out) ch_open_out(h, c, PSM_HIDCTL);           /* it called us: it opens the channels itself */
}
static void found_update(struct bt_host *h, const uint8_t a[6], uint8_t psr, uint32_t cod, uint16_t clk, int8_t rssi, const uint8_t *eir, unsigned eirn)
{
    if (!kind_of_cod(cod) && ((cod >> 8) & 0x1f) != 5) return;     /* keyboards, mice and other peripherals only */
    struct bt_found *f = found_of(h, a);
    if (!f) {
        if (h->nfound >= BT_MAXFOUND) return;
        f = &h->found[h->nfound++];
        mset(f, 0, sizeof *f);
        mcpy(f->addr, a, 6);
        lg(h, "found %a, class %x", a, cod);
    }
    f->psr = psr; f->cod = cod; f->clkoff = clk; f->rssi = rssi;
    for (unsigned i = 0; eir && i + 1 < eirn; ) {      /* the name in the Extended Inquiry Response */
        unsigned l = eir[i];
        if (!l || i + 1 + l > eirn) break;
        if (eir[i + 1] == 0x09 || (eir[i + 1] == 0x08 && !f->named)) {
            unsigned k = 0;
            for (; k + 1 < l && k < BT_NAME - 1; k++) f->name[k] = (char)eir[i + 2 + k];
            f->name[k] = 0;
            f->named = 1;
        }
        i += 1 + l;
    }
}
static void next_name(struct bt_host *h)
{
    for (int i = 0; i < h->nfound; i++) {
        struct bt_found *f = &h->found[i];
        if (f->named) continue;
        uint8_t p[10];
        mcpy(p, f->addr, 6); p[6] = f->psr; p[7] = 0; put16(p + 8, (unsigned)(f->clkoff | 0x8000));
        f->named = -1;                                  /* asked */
        h->names_pending = 1;
        cmd(h, C_REMOTE_NAME, p, 10);
        return;
    }
    h->names_pending = 0;
    if (!h->scanning) say(h, h->nfound ? "scan done" : "scan done - nothing found (is the device in pairing mode?)");
}

static void event(struct bt_host *h, const uint8_t *e, unsigned n)
{
    if (n < 2) return;
    uint8_t code = e[0];
    const uint8_t *p = e + 2;
    unsigned len = e[1];
    if (len + 2 > n) return;
    switch (code) {
    case 0x0e:                                          /* Command Complete */
        if (len < 3) return;
        h->credits = p[0];
        startup_answer(h, le16(p + 1), p + 3, len - 3);
        cmd_flush(h);
        return;
    case 0x0f:                                          /* Command Status */
        if (len < 4) return;
        h->credits = p[1];
        if (p[0]) {
            uint16_t op = le16(p + 2);
            lg(h, "command %x: status %u", op, p[0]);
            if (op == C_CREATE_CONN) { h->calling = 0; if (h->pairing) { h->pairing = 0; say(h, "could not reach the device"); } }
            if (op == C_INQUIRY) { h->scanning = 0; say(h, "scan refused"); }
            if (op == C_REMOTE_NAME) next_name(h);
            if (op == h->pending_op) startup_answer(h, op, p, 1);
        }
        cmd_flush(h);
        return;
    case 0x13:                                          /* Number Of Completed Packets */
        for (unsigned i = 0; i < p[0] && 1 + 4 * i + 4 <= len; i++) h->acl_free = (uint16_t)(h->acl_free + le16(p + 1 + 4 * i + 2));
        acl_flush(h);
        return;
    case 0x01:                                          /* Inquiry Complete */
        h->scanning = 0;
        lg(h, "scan done: %u found", (uint32_t)h->nfound);
        next_name(h);
        return;
    case 0x02: case 0x22: {                             /* Inquiry Result (with RSSI): fields in arrays */
        unsigned m = p[0], rs = code == 0x22;
        if (len < 1 + m * (rs ? 14u : 14u)) return;
        for (unsigned i = 0; i < m; i++) {
            const uint8_t *a = p + 1 + 6 * i;
            const uint8_t *q = p + 1 + 6 * m;
            uint8_t psr = q[i];
            q += m + (rs ? m : 2 * m);
            uint32_t cod = q[3 * i] | q[3 * i + 1] << 8 | (uint32_t)q[3 * i + 2] << 16;
            q += 3 * m;
            uint16_t clk = le16(q + 2 * i);
            int8_t rssi = rs ? (int8_t)(q + 2 * m)[i] : 0;
            found_update(h, a, psr, cod, clk, rssi, 0, 0);
        }
        return;
    }
    case 0x2f:                                          /* Extended Inquiry Result: one, with EIR */
        if (len < 15) return;
        found_update(h, p + 1, p[7], p[9] | p[10] << 8 | (uint32_t)p[11] << 16, le16(p + 12), (int8_t)p[14], p + 15, len - 15);
        return;
    case 0x07: {                                        /* Remote Name Request Complete */
        if (len < 7) return;
        struct bt_found *f = found_of(h, p + 1);
        if (f) {
            f->named = 1;
            if (!p[0]) { unsigned k = 0; for (; k < BT_NAME - 1 && 7 + k < len && p[7 + k]; k++) f->name[k] = (char)p[7 + k]; f->name[k] = 0; }
        }
        next_name(h);
        return;
    }
    case 0x04: {                                        /* Connection Request */
        if (len < 10) return;
        const uint8_t *a = p;
        int known = key_of(h, a) || (h->pairing && !mcmp(h->pair_addr, a, 6));
        uint8_t r[7];
        mcpy(r, a, 6);
        if (known && p[9] == 1) { r[6] = 0x00; cmd(h, C_ACCEPT_CONN, r, 7); lg(h, "%a connects", a); }   /* we become master */
        else { r[6] = 0x0f; cmd(h, C_REJECT_CONN, r, 7); lg(h, "%a refused: not paired", a); }
        return;
    }
    case 0x03: {                                        /* Connection Complete */
        if (len < 11) return;
        int ours = h->calling && !mcmp(h->call_addr, p + 3, 6);
        if (ours) h->calling = 0;
        if (p[0]) {
            lg(h, "%a: no connection (%u)", p + 3, p[0]);
            if (h->pairing && !mcmp(h->pair_addr, p + 3, 6)) { h->pairing = 0; say(h, "could not connect - is it in pairing mode?"); }
            h->reconnect_at = now(h) + 200;
            return;
        }
        struct bt_conn *c = 0;
        for (int i = 0; i < BT_MAXCONN && !c; i++) if (!h->conn[i].used) c = &h->conn[i];
        if (!c) { cmd_handle(h, C_DISCONNECT, le16(p + 1)); return; }
        mset(c, 0, sizeof *c);
        c->used = 1;
        c->handle = le16(p + 1) & 0x0fff;
        mcpy(c->addr, p + 3, 6);
        c->remote_io = 0xff;
        c->out = ours;
        c->pairing = h->pairing && !mcmp(h->pair_addr, c->addr, 6);
        lg(h, "%a: link up (handle %x, %s)", c->addr, c->handle, c->out ? "we called" : "it called");
        uint8_t ap[3];
        put16(ap, c->handle); ap[2] = 0;
        cmd(h, C_AUTH_REQ, ap, 2);
        h->reconnect_at = now(h) + 1000;
        return;
    }
    case 0x05: {                                        /* Disconnection Complete */
        if (len < 4) return;
        struct bt_conn *c = conn_of(h, le16(p + 1) & 0x0fff);
        if (!c) return;
        lg(h, "%a: disconnected (%u)", c->addr, p[3]);
        static const uint8_t up[8];
        int held = 0;
        for (int i = 0; i < 8; i++) held |= c->lastkb[i];
        if (held && h->io->keyboard) h->io->keyboard(h->io->ctx, up);    /* nothing stays pressed */
        set_connected(h, c->addr, 0);
        if (c->pairing && h->pairing) { h->pairing = 0; say(h, "pairing did not finish"); }
        else say(h, "disconnected");
        c->used = 0;
        return;
    }
    case 0x17: {                                        /* Link Key Request */
        if (len < 6) return;
        struct bt_key *k = key_of(h, p);
        struct bt_conn *c = conn_by_addr(h, p);
        if (k && !(c && c->pairing)) { uint8_t r[22]; mcpy(r, p, 6); mcpy(r + 6, k->key, 16); cmd(h, C_LINKKEY_REPLY, r, 22); }
        else cmd_addr(h, C_LINKKEY_NEG, p);
        return;
    }
    case 0x16: {                                        /* PIN Code Request: legacy pairing, 0000 */
        if (len < 6) return;
        uint8_t r[23];
        mset(r, 0, sizeof r);
        mcpy(r, p, 6); r[6] = 4; r[7] = r[8] = r[9] = r[10] = '0';
        cmd(h, C_PIN_REPLY, r, 23);
        lg(h, "%a: PIN 0000", p);
        return;
    }
    case 0x32: {                                        /* IO Capability Response: what the device can do */
        if (len < 9) return;
        struct bt_conn *c = conn_by_addr(h, p);
        if (c) c->remote_io = p[6];
        return;
    }
    case 0x31: {                                        /* IO Capability Request */
        if (len < 6) return;
        struct bt_conn *c = conn_by_addr(h, p);
        struct bt_found *f = found_of(h, p);
        struct bt_key *k = key_of(h, p);
        uint32_t kind = f ? kind_of_cod(f->cod) : k ? k->flags : 0;
        int noio = (c && c->remote_io == 0x03) || (!(kind & BTK_KEYBOARD) && !(c && c->remote_io == 0x02));
        uint8_t r[9];
        mcpy(r, p, 6);
        r[6] = 0x00;                                    /* DisplayOnly: we show a passkey, the keyboard types it */
        r[7] = 0x00;
        r[8] = noio ? 0x04 : 0x05;                      /* general bonding; MITM when the device can type */
        cmd(h, C_IOCAP_REPLY, r, 9);
        return;
    }
    case 0x33:                                          /* User Confirmation Request: Just Works */
        if (len < 6) return;
        cmd_addr(h, C_CONFIRM_REPLY, p);
        return;
    case 0x3b:                                          /* User Passkey Notification */
        if (len < 10) return;
        h->passkey = le32(p + 6);
        h->passkey_shown = 1;
        {
            char s[64] = "type ";
            unsigned k = 5;
            uint32_t v = h->passkey;
            char d[6];
            for (int i = 5; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; }
            for (int i = 0; i < 6; i++) s[k++] = d[i];
            for (const char *q = " on the keyboard, then Enter"; *q; q++) s[k++] = *q;
            s[k] = 0;
            say(h, s);
        }
        lg(h, "%a: passkey %06u", p, h->passkey);
        return;
    case 0x36:                                          /* Simple Pairing Complete */
        if (len < 7) return;
        h->passkey_shown = 0;
        if (p[0]) { lg(h, "%a: pairing failed (%u)", p + 1, p[0]); say(h, "pairing failed"); }
        return;
    case 0x18: {                                        /* Link Key Notification: a new pairing */
        if (len < 23) return;
        struct bt_found *f = found_of(h, p);
        if (bt_add_key(h, p, p + 6, p[22], f ? kind_of_cod(f->cod) : 0)) { lg(h, "%a: paired, but no room to keep it", p); return; }
        struct bt_key *k = key_of(h, p);
        if (f && f->name[0]) { unsigned i = 0; for (; f->name[i] && i < BT_NAME - 1; i++) k->name[i] = f->name[i]; k->name[i] = 0; }
        lg(h, "%a: paired (key type %u)", p, p[22]);
        say(h, "paired");
        keys_changed(h);
        return;
    }
    case 0x06: {                                        /* Authentication Complete */
        if (len < 3) return;
        struct bt_conn *c = conn_of(h, le16(p + 1) & 0x0fff);
        if (!c) return;
        if (p[0]) {
            lg(h, "%a: authentication failed (%u)", c->addr, p[0]);
            if (c->pairing) { h->pairing = 0; say(h, "pairing failed"); }
            cmd_handle(h, C_DISCONNECT, c->handle);
            return;
        }
        c->authed = 1;
        uint8_t r[3]; put16(r, c->handle); r[2] = 1;
        cmd(h, C_SET_ENCRYPT, r, 3);
        return;
    }
    case 0x08: {                                        /* Encryption Change */
        if (len < 4) return;
        struct bt_conn *c = conn_of(h, le16(p + 1) & 0x0fff);
        if (!c) return;
        if (p[0] || !p[3]) { lg(h, "%a: not encrypted (%u)", c->addr, p[0]); return; }
        if (c->encrypted) return;
        c->encrypted = 1;
        if (c->pairing) { c->pairing = 0; h->pairing = 0; }
        open_conn_done(h, c);
        return;
    }
    default:
        return;                                         /* role changes, slot changes, vendor events */
    }
}

/* ---- the H4 receiver ------------------------------------------------------------ */
void bt_rx(struct bt_host *h, const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (!h->rx_type) {
            if (b == 0x04) { h->rx_type = 4; h->rx_need = 2; h->rx_n = 0; }
            else if (b == 0x02) { h->rx_type = 2; h->rx_need = 4; h->rx_n = 0; }
            continue;                                   /* anything else: out of step, wait for a start */
        }
        if (h->rx_n < sizeof h->rx) h->rx[h->rx_n] = b;
        h->rx_n++;
        if (h->rx_n == h->rx_need) {
            if (h->rx_type == 4 && h->rx_n == 2) { h->rx_need = 2u + h->rx[1]; if (h->rx_need > 2) continue; }
            else if (h->rx_type == 2 && h->rx_n == 4) {
                h->rx_need = 4u + le16(h->rx + 2);
                if (h->rx_need > sizeof h->rx) { h->rx_type = 0; continue; }
                if (h->rx_need > 4) continue;
            }
            if (h->rx_type == 4) event(h, h->rx, h->rx_n); else acl_in(h, h->rx, h->rx_n);
            h->rx_type = 0;
        }
    }
}

void bt_poll(struct bt_host *h)
{
    if (h->state == BT_FAILED || h->state == BT_OFF) return;
    if (h->pending_op && due(h, h->wait_until) && h->state == BT_FW && (h->step == S_ROM_VER || h->step == S_ROM_NAME)) {
        lg(h, "no answer to %x from the chip's ROM (going on)", h->pending_op);
        h->pending_op = 0; h->credits = 1;
        after_rom(h);
        return;
    }
    if (h->pending_op && due(h, h->wait_until) && h->state == BT_FW && h->step == S_RESET2 && h->tries < RESET2_TRIES) {
        /* the firmware is started but its reset went unanswered: the line may be stuck (bytes held
           in the UART behind CTS) - the host unsticks it, and the reset goes again */
        lg(h, "no answer to the reset after the firmware (try %u of %u)", (uint32_t)h->tries, (uint32_t)RESET2_TRIES);
        if (h->io->kick) h->io->kick(h->io->ctx, h->tries);
        h->tries++;
        h->credits = 1;
        sync_cmd(h, C_RESET, 0, 0);
        h->wait_until = now(h) + 2000;
        return;
    }
    if (h->pending_op && due(h, h->wait_until)) {
        static const char hx[] = "0123456789abcdef";
        char why[64];
        unsigned k = 0;
        const char *w0 = h->state == BT_FW && h->step == S_RESET ? "the chip does not answer" :
                         h->state == BT_FW && h->step == S_RESET2 ? "its new firmware does not answer" : "the chip stopped answering";
        for (; w0[k]; k++) why[k] = w0[k];
        const char *w1 = " (command ";
        for (unsigned i = 0; w1[i]; i++) why[k++] = w1[i];
        for (int sh = 12; sh >= 0; sh -= 4) why[k++] = hx[(h->pending_op >> sh) & 15];
        if (h->state == BT_FW && h->step == S_PATCH) {             /* ", record N" */
            for (const char *q = ", record "; *q; ) why[k++] = *q++;
            char d[6]; int n = 0; uint32_t v = h->fwrec;
            do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 5);
            while (n) why[k++] = d[--n];
        } else if (h->state == BT_INIT) {                          /* ", set-up step N" */
            for (const char *q = ", set-up "; *q; ) why[k++] = *q++;
            why[k++] = (char)('0' + (h->step % 10));
        }
        why[k++] = ')'; why[k] = 0;
        if (h->state == BT_FW && h->step == S_PATCH) lg(h, "no answer to firmware record %u (command %x, byte %u of %u)", h->fwrec, h->pending_op, h->fwpos, h->fwlen);
        else lg(h, "no answer to command %x (start-up step %u)", h->pending_op, (uint32_t)h->step);
        fail(h, why);
        return;
    }
    if (h->state == BT_FW && !h->pending_op) {
        if (h->step == S_MINI_WAIT && due(h, h->wait_until)) { h->step = S_PATCH; h->fwpos = 0; h->fwrec = 0; say(h, "loading its firmware"); lg(h, "firmware: %u bytes", h->fwlen); patch_next(h); }
        else if (h->step == S_LAUNCH_WAIT && due(h, h->wait_until)) { h->step = S_RESET2; h->tries = 1; h->credits = 1; sync_cmd(h, C_RESET, 0, 0); }
        else if (h->step == S_RESET_PAUSE && due(h, h->wait_until)) {
            if (h->fw) { h->step = S_ROM_VER; sync_cmd(h, C_LOCAL_VERSION, 0, 0); h->wait_until = now(h) + 1000; }
            else { h->state = BT_INIT; h->step = 0; setup_next(h); }
        }
        else if (h->step == S_RESET2_PAUSE && due(h, h->wait_until)) { h->state = BT_INIT; h->step = 0; setup_next(h); }
    }
    if (h->state != BT_READY) return;
    if (h->scanning && due(h, h->scan_end)) h->scanning = 0;
    /* paired devices: call each once after the start (they also call us) */
    if (!h->pairing && !h->scanning && !h->names_pending && !h->calling && h->reconnect_next < h->nkeys && due(h, h->reconnect_at)) {
        struct bt_key *k = &h->keys[h->reconnect_next++];
        if (!conn_by_addr(h, k->addr)) { lg(h, "calling %a", k->addr); create_conn(h, k->addr); h->reconnect_at = now(h) + 6000; }
    }
}

/* ---- what the user asks ------------------------------------------------------------ */
int bt_scan(struct bt_host *h)
{
    if (h->state != BT_READY) return -1;
    if (h->scanning) return 0;
    h->nfound = 0;
    uint8_t p[5] = { 0x33, 0x8b, 0x9e, 0x08, 0x00 };   /* GIAC, 10.24 s, no limit */
    cmd(h, C_INQUIRY, p, 5);
    h->scanning = 1;
    h->scan_end = now(h) + 12000;
    say(h, "scanning - put the device in pairing mode");
    return 0;
}
int bt_pair(struct bt_host *h, const uint8_t a[6])
{
    if (h->state != BT_READY) return -1;
    if (h->scanning) { cmd(h, C_INQUIRY_CANCEL, 0, 0); h->scanning = 0; }
    struct bt_conn *c = conn_by_addr(h, a);
    mcpy(h->pair_addr, a, 6);
    h->pairing = 1;
    h->passkey_shown = 0;
    say(h, "pairing...");
    lg(h, "pairing with %a", a);
    if (c) { uint8_t ap[2]; put16(ap, c->handle); c->pairing = 1; cmd(h, C_AUTH_REQ, ap, 2); }
    else create_conn(h, a);
    return 0;
}
int bt_connect(struct bt_host *h, const uint8_t a[6])
{
    if (h->state != BT_READY || !key_of(h, a)) return -1;
    if (conn_by_addr(h, a)) return 0;
    h->reconnect_next = h->nkeys;                       /* the start-up's calls are over */
    create_conn(h, a);
    return 0;
}
int bt_forget(struct bt_host *h, const uint8_t a[6])
{
    struct bt_conn *c = conn_by_addr(h, a);
    if (c && h->state == BT_READY) { uint8_t r[3]; put16(r, c->handle); r[2] = 0x13; cmd(h, C_DISCONNECT, r, 3); }
    for (int i = 0; i < h->nkeys; i++)
        if (!mcmp(h->keys[i].addr, a, 6)) {
            for (int j = i; j + 1 < h->nkeys; j++) h->keys[j] = h->keys[j + 1];
            h->nkeys--;
            keys_changed(h);
            lg(h, "%a forgotten", a);
            return 0;
        }
    return -1;
}
