/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 kernel Bluetooth: LE keyboards and mice (HID over GATT). Included by
 * bthost.c, not compiled on its own: it uses bthost.c's helpers (cmd, l2_send,
 * lg, the key and link tables) and bthost.c's events call in here.
 *
 *   Link:  LE Create Connection - to one device when the user pairs it, or to
 *          the controller's filter accept list (every bonded LE device; the
 *          resolving list holds their IRKs) so a bonded keyboard or mouse that
 *          advertises is connected by the controller itself.
 *   SMP:   legacy pairing as initiator: Just Works, or Passkey Entry where we
 *          display a number and the keyboard types it. c1 and s1 are worked out
 *          with the controller's LE Encrypt (AES-128), one block at a time, so
 *          they run as a small state machine. Bonding keeps the device's LTK,
 *          EDIV and Rand (and its IRK and identity address); a later link is
 *          encrypted with them (LE Start Encryption). Secure Connections is not
 *          done yet: a device that insists on it is named in the log.
 *   GATT:  MTU, the HID service (0x1812), its characteristics and descriptors;
 *          boot protocol (Protocol Mode = 0, Boot Keyboard/Mouse Input Report
 *          notifications) when the device has boot reports, else report mode:
 *          Report References and the Report Map read, the keyboard and mouse
 *          reports found in it (the mouse with alice_hid.c, the USB driver's).
 *
 * Written from the Bluetooth Core Specification (Vol 3 Part A: L2CAP LE
 * signalling; Part F: ATT; Part G: GATT; Part H: SMP; Vol 4 Part E: HCI) and
 * the HID over GATT Profile / HID Service specifications. Every field is read
 * and written byte by byte; values are little-endian as on the air.
 */

/* the USB driver's mouse report parser, under other names (frf_xhci.c has it too) */
#define hid_mouse_parse  bt_hid_mouse_parse
#define hid_mouse_decode bt_hid_mouse_decode
#include "../../frf_usb/xhci/alice_hid.c"
#undef hid_mouse_parse
#undef hid_mouse_decode
_Static_assert(sizeof(struct hid_mouse) <= sizeof(((struct bt_le *)0)->hmouse), "bt_le.hmouse too small");

#define CID_ATT   0x0004
#define CID_LESIG 0x0005
#define CID_SMP   0x0006

/* SMP states (initiator) */
enum { SM_IDLE, SM_REQ, SM_RAND_PK, SM_RAND1, SM_RAND2, SM_C1A, SM_C1B, SM_WAIT_CONF, SM_WAIT_RAND,
       SM_V1A, SM_V1B, SM_S1, SM_ENC, SM_KEYS, SM_DONE, SM_FAILED };
/* GATT client steps */
enum { G_IDLE, G_MTU, G_SVC, G_CHR, G_DESC, G_REFS, G_MAP, G_CCCD, G_UP, G_FAIL };

#define U_HID_SVC   0x1812
#define U_PROTO     0x2a4e
#define U_BOOT_KB   0x2a22
#define U_BOOT_MS   0x2a33
#define U_REPORT    0x2a4d
#define U_MAP       0x2a4b
#define U_CCCD      0x2902
#define U_REPREF    0x2908

static void smp_send(struct bt_host *h, struct bt_conn *c, const uint8_t *d, unsigned n) { l2_send(h, c, CID_SMP, d, n); }
static void att_send(struct bt_host *h, struct bt_conn *c, const uint8_t *d, unsigned n) { l2_send(h, c, CID_ATT, d, n); }
static void le_disconnect(struct bt_host *h, struct bt_conn *c, uint8_t reason) { uint8_t r[3]; put16(r, c->handle); r[2] = reason; cmd(h, C_DISCONNECT, r, 3); }
static int le_kind_keyboard(struct bt_host *h, const uint8_t a[6])
{
    struct bt_found *f = found_of(h, a);
    struct bt_key *k = key_of(h, a);
    uint32_t kind = f ? kind_of_cod(f->cod) : k ? k->flags : 0;
    return (kind & BTK_KEYBOARD) != 0;
}

/* ---- keys -------------------------------------------------------------------------- */
int bt_add_le_key(struct bt_host *h, const uint8_t a[6], uint8_t addr_type, const uint8_t ltk[16], uint16_t ediv,
                  const uint8_t rand[8], const uint8_t *irk, uint32_t kind)
{
    struct bt_key *k = key_of(h, a);
    if (!k) {
        if (h->nkeys >= BT_MAXKEY) return -1;
        k = &h->keys[h->nkeys++];
        mset(k, 0, sizeof *k);
        mcpy(k->addr, a, 6);
    }
    mcpy(k->key, ltk, 16);
    k->type = 0;
    k->addr_type = addr_type & 1;
    k->ediv = ediv;
    mcpy(k->rand, rand, 8);
    k->has_irk = irk != 0;
    if (irk) mcpy(k->irk, irk, 16); else mset(k->irk, 0, 16);
    k->flags = (k->flags & BTK_CONNECTED) | BTK_LE | (kind & (BTK_KEYBOARD | BTK_MOUSE));
    h->le_lists_dirty = 1;
    return 0;
}
static int le_keys_waiting(struct bt_host *h)       /* bonded LE devices not connected now */
{
    for (int i = 0; i < h->nkeys; i++) if ((h->keys[i].flags & BTK_LE) && !conn_by_addr(h, h->keys[i].addr)) return 1;
    return 0;
}

/* ---- the link ---------------------------------------------------------------------- */
static void le_create(struct bt_host *h, int policy, uint8_t type, const uint8_t a[6])
{
    uint8_t p[25];
    put16(p, 0x0060); put16(p + 2, 0x0030);          /* scan 60 ms, window 30 ms */
    p[4] = (uint8_t)policy;                          /* 0: this device, 1: the filter accept list */
    p[5] = type;
    if (a) mcpy(p + 6, a, 6); else mset(p + 6, 0, 6);
    p[12] = 0;                                       /* our public address */
    put16(p + 13, 0x0006); put16(p + 15, 0x000c);    /* 7.5 to 15 ms: what keyboards ask for */
    put16(p + 17, 0);                                /* no latency */
    put16(p + 19, 0x01f4);                           /* supervision 5 s */
    put16(p + 21, 0); put16(p + 23, 0);
    cmd(h, C_LE_CREATE, p, 25);
}
static void le_cancel(struct bt_host *h)
{
    if (!h->le_init || h->le_cancelling) return;
    cmd(h, C_LE_CREATE_CANCEL, 0, 0);
    h->le_cancelling = 1;
    h->le_init = 0;
}
static void le_connect_direct(struct bt_host *h, const uint8_t a[6], uint8_t type)
{
    le_scan_stop(h);
    le_cancel(h);
    le_create(h, 0, type, a);
    h->le_init = 1;
    h->le_init_until = now(h) + 15000;
}
/* the accept list (and resolving list) from the bonded LE devices */
static void le_lists_write(struct bt_host *h)
{
    int irks = 0;
    cmd(h, C_LE_ACC_CLEAR, 0, 0);
    for (int i = 0; i < h->nkeys; i++) {
        struct bt_key *k = &h->keys[i];
        if (!(k->flags & BTK_LE)) continue;
        uint8_t p[7]; p[0] = k->addr_type; mcpy(p + 1, k->addr, 6);
        cmd(h, C_LE_ACC_ADD, p, 7);
        irks += k->has_irk;
    }
    uint8_t z = 0;
    cmd(h, C_LE_RES_ENABLE, &z, 1);                  /* the resolving list can only change while it is off */
    cmd(h, C_LE_RES_CLEAR, 0, 0);
    if (irks) {
        for (int i = 0; i < h->nkeys; i++) {
            struct bt_key *k = &h->keys[i];
            if (!(k->flags & BTK_LE) || !k->has_irk) continue;
            uint8_t p[39]; p[0] = k->addr_type; mcpy(p + 1, k->addr, 6); mcpy(p + 7, k->irk, 16); mset(p + 23, 0, 16);
            cmd(h, C_LE_RES_ADD, p, 39);
        }
        z = 1;
        cmd(h, C_LE_RES_ENABLE, &z, 1);
    }
    h->le_lists_dirty = 0;
}

/* ---- SMP --------------------------------------------------------------------------- */
static void aes(struct bt_host *h, struct bt_conn *c, const uint8_t key[16], const uint8_t pt[16])
{
    uint8_t p[32];
    mcpy(p, key, 16); mcpy(p + 16, pt, 16);          /* HCI: least significant octet first, as SMP's values */
    h->aes_busy = 1; h->aes_handle = c->handle;
    cmd(h, C_LE_ENCRYPT, p, 32);
}
static void rnd(struct bt_host *h, struct bt_conn *c) { h->aes_busy = 1; h->aes_handle = c->handle; cmd(h, C_LE_RAND, 0, 0); }

static const char *smp_reason(uint8_t r)
{
    switch (r) {
    case 0x01: return "passkey entry failed";
    case 0x02: return "OOB not available";
    case 0x03: return "authentication requirements (it may want Secure Connections, not done yet)";
    case 0x04: return "confirm value failed (wrong passkey?)";
    case 0x05: return "pairing not supported";
    case 0x06: return "encryption key size";
    case 0x07: return "command not supported";
    case 0x08: return "unspecified reason";
    case 0x09: return "repeated attempts: wait a little";
    case 0x0a: return "invalid parameters";
    case 0x0b: return "DHKey check failed";
    case 0x0c: return "numeric comparison failed";
    default:   return "?";
    }
}
static void smp_fail(struct bt_host *h, struct bt_conn *c, uint8_t send_reason, const char *why)
{
    if (send_reason) { uint8_t d[2] = { 0x05, send_reason }; smp_send(h, c, d, 2); }
    c->l.smp = SM_FAILED;
    lg(h, "%a: LE pairing failed: %s", c->addr, why);
    if (c->pairing || h->pairing) { h->pairing = 0; h->passkey_shown = 0; c->pairing = 0; }
    say(h, why);
    le_disconnect(h, c, 0x05);
}
static void smp_start(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    l->mitm = (uint8_t)le_kind_keyboard(h, c->addr);
    l->preq[0] = 0x01;                               /* Pairing Request */
    l->preq[1] = 0x00;                               /* DisplayOnly: we show a passkey, a keyboard types it */
    l->preq[2] = 0x00;                               /* no OOB */
    l->preq[3] = (uint8_t)(0x01 | (l->mitm ? 0x04 : 0));   /* bonding, MITM for keyboards (no SC: legacy) */
    l->preq[4] = 16;
    l->preq[5] = 0x00;                               /* we hand out no keys (our address is public) */
    l->preq[6] = 0x03;                               /* its LTK (EncKey) and identity (IdKey) */
    l->smp = SM_REQ;
    l->have_sconf = l->have_srand = 0; l->got = 0;
    l->smp_until = now(h) + 30000;
    smp_send(h, c, l->preq, 7);
    lg(h, "%a: LE pairing (%s)", c->addr, l->mitm ? "keyboard: passkey if it can type" : "Just Works");
}
/* c1 first half: e(k, r XOR p1); p1 = pres || preq || rat || iat (least significant first) */
static void c1_first(struct bt_host *h, struct bt_conn *c, const uint8_t r[16])
{
    struct bt_le *l = &c->l;
    uint8_t p1[16];
    p1[0] = 0x00;                                    /* iat: we use our public address */
    p1[1] = l->rat;
    mcpy(p1 + 2, l->preq, 7);
    mcpy(p1 + 9, l->pres, 7);
    for (int i = 0; i < 16; i++) p1[i] ^= r[i];
    aes(h, c, l->tk, p1);
}
/* c1 second half: e(k, x XOR p2); p2 = padding || ia || ra */
static void c1_second(struct bt_host *h, struct bt_conn *c, const uint8_t x[16])
{
    struct bt_le *l = &c->l;
    uint8_t p2[16];
    mcpy(p2, l->ra, 6);
    mcpy(p2 + 6, h->bdaddr, 6);
    mset(p2 + 12, 0, 4);
    for (int i = 0; i < 16; i++) p2[i] ^= x[i];
    aes(h, c, l->tk, p2);
}
static void smp_send_random(struct bt_host *h, struct bt_conn *c)
{
    uint8_t d[17]; d[0] = 0x04; mcpy(d + 1, c->l.mrand, 16);
    smp_send(h, c, d, 17);
    c->l.smp = SM_WAIT_RAND;
    if (c->l.have_srand) { c->l.smp = SM_V1A; c1_first(h, c, c->l.srand); }
}
static void show_passkey(struct bt_host *h)
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
static void le_start_enc(struct bt_host *h, struct bt_conn *c, const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16])
{
    uint8_t p[28];
    put16(p, c->handle); mcpy(p + 2, rand, 8); put16(p + 10, ediv); mcpy(p + 12, ltk, 16);
    cmd(h, C_LE_START_ENC, p, 28);
}
/* an LE Encrypt or LE Rand answer for the pairing on this link */
static void smp_crypto(struct bt_host *h, struct bt_conn *c, const uint8_t *o)
{
    struct bt_le *l = &c->l;
    switch (l->smp) {
    case SM_RAND_PK:
        h->passkey = le32(o) % 1000000u;
        h->passkey_shown = 1;
        mset(l->tk, 0, 16); l->tk[0] = (uint8_t)h->passkey; l->tk[1] = (uint8_t)(h->passkey >> 8); l->tk[2] = (uint8_t)(h->passkey >> 16);
        show_passkey(h);
        lg(h, "%a: passkey %06u", c->addr, h->passkey);
        l->smp = SM_RAND1; rnd(h, c);
        return;
    case SM_RAND1: mcpy(l->mrand, o, 8); l->smp = SM_RAND2; rnd(h, c); return;
    case SM_RAND2: mcpy(l->mrand + 8, o, 8); l->smp = SM_C1A; c1_first(h, c, l->mrand); return;
    case SM_C1A: l->smp = SM_C1B; c1_second(h, c, o); return;
    case SM_C1B: {
        mcpy(l->conf, o, 16);
        uint8_t d[17]; d[0] = 0x03; mcpy(d + 1, l->conf, 16);
        smp_send(h, c, d, 17);
        l->smp = SM_WAIT_CONF;
        if (l->have_sconf) smp_send_random(h, c);
        return;
    }
    case SM_V1A: l->smp = SM_V1B; c1_second(h, c, o); return;
    case SM_V1B: {
        if (mcmp(o, l->sconf, 16)) { smp_fail(h, c, 0x04, "the device's confirm value does not match (wrong passkey?)"); return; }
        uint8_t r[16];                               /* s1(TK, Srand, Mrand): r' = Srand[0..7] || Mrand[0..7] */
        mcpy(r, l->mrand, 8); mcpy(r + 8, l->srand, 8);
        l->smp = SM_S1;
        aes(h, c, l->tk, r);
        return;
    }
    case SM_S1: {
        uint8_t stk[16], z[8];
        mcpy(stk, o, 16);
        for (unsigned i = l->keysize; i < 16; i++) stk[i] = 0;    /* a shorter key: the high octets zero */
        mset(z, 0, 8);
        l->smp = SM_ENC;
        h->passkey_shown = 0;
        le_start_enc(h, c, z, 0, stk);
        return;
    }
    default:
        return;
    }
}
static void le_bonded(struct bt_host *h, struct bt_conn *c);
static void gatt_start(struct bt_host *h, struct bt_conn *c);
static void smp_keys_check(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    int need_enc = l->rdist & 1, need_id = (l->rdist >> 1) & 1;
    if (need_enc && (l->got & 3) != 3) return;
    if (need_id && (l->got & 12) != 12) return;
    l->smp = SM_DONE;
    le_bonded(h, c);
}
static void smp_in(struct bt_host *h, struct bt_conn *c, const uint8_t *d, unsigned n)
{
    struct bt_le *l = &c->l;
    if (!n) return;
    switch (d[0]) {
    case 0x02:                                       /* Pairing Response */
        if (n < 7 || l->smp != SM_REQ) return;
        mcpy(l->pres, d, 7);
        l->keysize = d[4] < 16 ? d[4] : 16;
        if (l->keysize < 7) { smp_fail(h, c, 0x06, "the device's key is too short"); return; }
        l->rdist = d[6] & 0x03;
        if (d[3] & 0x08) lg(h, "%a offers Secure Connections; legacy pairing used", c->addr);
        {
            uint8_t rio = d[1];
            int mitm = ((l->preq[3] | d[3]) & 0x04) != 0;
            l->passkey_method = (uint8_t)(mitm && (rio == 0x02 || rio == 0x04));   /* it can type: KeyboardOnly or KeyboardDisplay */
            lg(h, "%a: pairing response io %u auth %x key %u: %s", c->addr, rio, d[3], l->keysize, l->passkey_method ? "passkey" : "Just Works");
        }
        mset(l->tk, 0, 16);
        if (l->passkey_method) { l->smp = SM_RAND_PK; rnd(h, c); }
        else { l->smp = SM_RAND1; rnd(h, c); }
        return;
    case 0x03:                                       /* Pairing Confirm */
        if (n < 17) return;
        mcpy(l->sconf, d + 1, 16); l->have_sconf = 1;
        if (l->smp == SM_WAIT_CONF) smp_send_random(h, c);
        return;
    case 0x04:                                       /* Pairing Random */
        if (n < 17) return;
        mcpy(l->srand, d + 1, 16); l->have_srand = 1;
        if (l->smp == SM_WAIT_RAND) { l->smp = SM_V1A; c1_first(h, c, l->srand); }
        return;
    case 0x05:                                       /* Pairing Failed */
        if (n < 2) return;
        if (l->smp == SM_IDLE || l->smp == SM_DONE || l->smp == SM_FAILED) return;
        smp_fail(h, c, 0, d[1] == 0x03 || (l->pres[3] & 0x08) ? "it wants Secure Connections pairing: not supported yet" : smp_reason(d[1]));
        lg(h, "%a: the device stopped the pairing: %s (%u)", c->addr, smp_reason(d[1]), d[1]);
        return;
    case 0x06: if (n >= 17) { mcpy(l->ltk, d + 1, 16); l->got |= 1; smp_keys_check(h, c); } return;        /* Encryption Information */
    case 0x07: if (n >= 11) { l->ediv = le16(d + 1); mcpy(l->rand, d + 3, 8); l->got |= 2; smp_keys_check(h, c); } return;   /* Master Identification */
    case 0x08: if (n >= 17) { mcpy(l->irk, d + 1, 16); l->got |= 4; smp_keys_check(h, c); } return;        /* Identity Information */
    case 0x09: if (n >= 8) { mcpy(l->idaddr, d + 1, 7); l->got |= 8; smp_keys_check(h, c); } return;       /* Identity Address Information */
    case 0x0a: return;                               /* Signing Information: not asked for */
    case 0x0b:                                       /* Security Request: the device wants encryption */
        if (l->smp != SM_IDLE || l->rekey || c->encrypted) return;
        if (key_of(h, c->addr) && (key_of(h, c->addr)->flags & BTK_LE)) { struct bt_key *k = key_of(h, c->addr); l->rekey = 1; le_start_enc(h, c, k->rand, k->ediv, k->key); }
        else if (c->pairing) smp_start(h, c);
        return;
    default: {
        uint8_t r[2] = { 0x05, 0x07 };               /* Command Not Supported */
        smp_send(h, c, r, 2);
        return;
    }
    }
}
/* the pairing is over and its keys are in: keep them, then the HID service */
static void le_bonded(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    struct bt_found *f = found_of(h, c->addr);
    uint32_t kind = f ? kind_of_cod(f->cod) : 0;
    if (l->got & 8) {                                /* its identity address: the one it is known by */
        uint8_t ida[6]; mcpy(ida, l->idaddr + 1, 6);
        if (mcmp(ida, c->addr, 6)) { lg(h, "%a: identity address %a", c->addr, ida); mcpy(c->addr, ida, 6); }
        l->addr_type = l->idaddr[0] & 1;
    }
    if ((l->got & 3) == 3) {
        if (bt_add_le_key(h, c->addr, l->addr_type, l->ltk, l->ediv, l->rand, (l->got & 4) ? l->irk : 0, kind))
            lg(h, "%a: paired, but no room to keep it", c->addr);
        else {
            struct bt_key *k = key_of(h, c->addr);
            if (f && f->name[0]) { unsigned i = 0; for (; f->name[i] && i < BT_NAME - 1; i++) k->name[i] = f->name[i]; k->name[i] = 0; }
            lg(h, "%a: paired (LE, key size %u%s)", c->addr, l->keysize, (l->got & 4) ? ", with its IRK" : "");
            keys_changed(h);
        }
    } else lg(h, "%a: paired for this time only (it gave no key to keep)", c->addr);
    say(h, "paired");
    c->pairing = 0; h->pairing = 0;
    gatt_start(h, c);
}
/* Encryption Change on an LE link */
static void le_enc_change(struct bt_host *h, struct bt_conn *c, uint8_t status, uint8_t on)
{
    struct bt_le *l = &c->l;
    if (status || !on) {
        if (l->smp == SM_ENC) { smp_fail(h, c, 0, "encryption with the new key failed"); return; }
        if (l->rekey) {
            lg(h, "%a: its key was refused (%u): pair it again", c->addr, status);
            say(h, status == 0x06 ? "the device forgot the pairing - pair it again" : "encryption failed - pair it again");
            le_disconnect(h, c, 0x05);
        }
        return;
    }
    if (c->encrypted) return;
    c->encrypted = 1;
    if (l->smp == SM_ENC) {                          /* pairing: its keys come now */
        l->smp = SM_KEYS;
        if (!l->rdist) { l->smp = SM_DONE; le_bonded(h, c); }
        else smp_keys_check(h, c);
        return;
    }
    lg(h, "%a: encrypted with its stored key", c->addr);
    gatt_start(h, c);
}

/* ---- a report-mode keyboard from the Report Map ------------------------------------ */
static void kb_parse(const uint8_t *d, unsigned len, struct bt_kbmap *k)
{
    uint32_t page = 0, rsize = 0, rcount = 0, umin = 0, umax = 0, usage = 0;
    uint8_t id = 0, nus = 0;
    uint16_t off[16]; unsigned offid[16]; unsigned noff = 0;
    int depth = 0, kbdepth = -1, have_mod = 0, have_keys = 0;
    struct { uint32_t page, rsize, rcount; uint8_t id; } st[4]; int sp = 0;
    mset(k, 0, sizeof *k);
    for (unsigned i = 0; i < len; ) {
        uint8_t p = d[i];
        if (p == 0xfe) { if (i + 1 >= len) break; i += 3u + d[i + 1]; continue; }
        unsigned n = (p & 3) == 3 ? 4 : (p & 3), type = (p >> 2) & 3, tag = p >> 4;
        if (i + 1 + n > len) break;
        uint32_t v = 0;
        for (unsigned b = 0; b < n; b++) v |= (uint32_t)d[i + 1 + b] << (8 * b);
        i += 1 + n;
        if (type == 1) {                             /* global */
            if (tag == 0) page = v;
            else if (tag == 7) rsize = v;
            else if (tag == 8) id = (uint8_t)v;
            else if (tag == 9) rcount = v;
            else if (tag == 10 && sp < 4) { st[sp].page = page; st[sp].rsize = rsize; st[sp].rcount = rcount; st[sp].id = id; sp++; }
            else if (tag == 11 && sp > 0) { sp--; page = st[sp].page; rsize = st[sp].rsize; rcount = st[sp].rcount; id = st[sp].id; }
        } else if (type == 2) {                      /* local */
            uint32_t pg = n == 4 ? v >> 16 : page, u = n == 4 ? v & 0xffff : v;
            if (tag == 0) { usage = pg << 16 | u; nus++; }
            else if (tag == 1) umin = u;
            else if (tag == 2) umax = u;
        } else if (type == 0) {                      /* main */
            if (tag == 0xa) {                        /* Collection */
                if (v == 1 && usage == (0x01u << 16 | 0x06) && kbdepth < 0) kbdepth = depth;   /* Application: Keyboard */
                depth++;
            } else if (tag == 0xc) { depth--; if (depth == kbdepth) kbdepth = -1; }
            else if (tag == 0x8) {                   /* Input */
                unsigned j = 0;
                for (; j < noff && offid[j] != id; j++) ;
                if (j == noff && noff < 16) { offid[noff] = id; off[noff] = 0; noff++; }
                uint16_t at = j < 16 ? off[j] : 0;
                if (kbdepth >= 0 && page == 0x07 && !(v & 1)) {
                    struct bt_kbrep *r = 0;
                    for (unsigned q = 0; q < k->n; q++) if (k->r[q].id == id) r = &k->r[q];
                    if (!r && k->n < 3) { r = &k->r[k->n++]; mset(r, 0, sizeof *r); r->id = id; }
                    if (r) {
                        if ((v & 2) && umin == 0xe0 && rsize == 1 && rcount == 8 && !r->has_mod) { r->mod_pos = at; r->has_mod = 1; }
                        else if (!(v & 2) && rsize == 8 && !r->nkeys) { r->keys_pos = at; r->nkeys = (uint8_t)(rcount > 6 ? 6 : rcount); }
                        else if ((v & 2) && rsize == 1 && rcount > 8 && !r->bm_n) { r->bm_pos = at; r->bm_min = (uint8_t)umin; r->bm_n = (uint8_t)(rcount > 255 ? 255 : rcount); }   /* NKRO */
                    }
                }
                if (j < 16) off[j] = (uint16_t)(at + rsize * rcount);
            }
            usage = 0; umin = umax = 0; nus = 0;
        }
    }
    (void)umax; (void)nus; (void)have_mod; (void)have_keys;
    for (unsigned q = 0; q < k->n; q++) if (k->r[q].nkeys || k->r[q].bm_n) k->ok = 1;
}
static unsigned bits(const uint8_t *r, unsigned n, unsigned pos, unsigned size)
{
    unsigned v = 0;
    for (unsigned b = 0; b < size; b++) { unsigned at = pos + b; if (at / 8 < n && (r[at / 8] >> (at % 8) & 1)) v |= 1u << b; }
    return v;
}

/* ---- GATT client ------------------------------------------------------------------- */
static void att_req3(struct bt_host *h, struct bt_conn *c, uint8_t op, uint16_t a, uint16_t b, uint16_t u)
{
    uint8_t d[7]; d[0] = op; put16(d + 1, a); put16(d + 3, b); put16(d + 5, u);
    att_send(h, c, d, op == 0x04 ? 5 : 7);
}
static int chr_find(struct bt_le *l, uint16_t uuid)
{
    for (int i = 0; i < l->nchr; i++) if (l->chr_uuid[i] == uuid) return i;
    return -1;
}
static void gatt_next_ref(struct bt_host *h, struct bt_conn *c);
static void gatt_next_cccd(struct bt_host *h, struct bt_conn *c);
static void gatt_start(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    l->nlog = 0;
    l->g = G_MTU; l->nchr = 0; l->pm = l->kb_in = l->ms_in = l->map = 0; l->boot = 0; l->mouse_ok = 0;
    mset(&l->kbm, 0, sizeof l->kbm);
    l->mtu = 23;
    uint8_t d[3] = { 0x02, 0, 0 }; put16(d + 1, 247);     /* Exchange MTU */
    att_send(h, c, d, 3);
}
static void gatt_up(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    l->g = G_UP;
    c->hid_up = 1;
    struct bt_key *k = key_of(h, c->addr);
    uint32_t kind = (l->kb_in || l->kbm.ok ? BTK_KEYBOARD : 0) | (l->ms_in || l->mouse_ok ? BTK_MOUSE : 0);
    if (k && (k->flags & (BTK_KEYBOARD | BTK_MOUSE)) != kind && kind) { k->flags = (k->flags & ~(BTK_KEYBOARD | BTK_MOUSE)) | kind; keys_changed(h); }
    set_connected(h, c->addr, 1);
    lg(h, "%a connected (LE HID, %s protocol%s%s)", c->addr, l->boot ? "boot" : "report", kind & BTK_KEYBOARD ? ", keyboard" : "", kind & BTK_MOUSE ? ", mouse" : "");
    say(h, kind & BTK_KEYBOARD ? "keyboard connected" : kind & BTK_MOUSE ? "mouse connected" : "device connected (no keyboard or mouse reports found)");
}
/* after the characteristics and descriptors: boot or report mode */
static void gatt_choose(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    int i;
    if ((i = chr_find(l, U_PROTO)) >= 0) l->pm = l->chr_vh[i];
    if ((i = chr_find(l, U_BOOT_KB)) >= 0) l->kb_in = l->chr_vh[i];
    if ((i = chr_find(l, U_BOOT_MS)) >= 0) l->ms_in = l->chr_vh[i];
    if ((i = chr_find(l, U_MAP)) >= 0) l->map = l->chr_vh[i];
    l->boot = 0;                                     /* the Report Map first: report mode when it is understood */
    l->g = G_REFS; l->gi = 0;
    gatt_next_ref(h, c);
}
/* report mode when the Report Map gave a keyboard or a mouse (many keyboards keep sending their
   reports, ignoring Protocol Mode = Boot); boot mode only when it did not and the device has boot
   reports. Notifications are asked for on both kinds either way. */
static void gatt_mode(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    l->boot = !(l->kbm.ok || l->mouse_ok) && (l->kb_in || l->ms_in) && l->pm;
    if (!l->boot && !l->pm) l->kb_in = l->ms_in = 0;          /* boot reports only mean something in boot mode */
    if (l->pm) { uint8_t d[4] = { 0x52, 0, 0, (uint8_t)(l->boot ? 0 : 1) }; put16(d + 1, l->pm); att_send(h, c, d, 4); }   /* Write Command: Protocol Mode */
    l->g = G_CCCD; l->gi = 0;
    gatt_next_cccd(h, c);
}
static void gatt_next_ref(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    for (; l->gi < l->nchr; l->gi++) {
        if (l->chr_uuid[l->gi] != U_REPORT || !l->chr_ref[l->gi]) continue;
        uint8_t d[3] = { 0x0a, 0, 0 }; put16(d + 1, l->chr_ref[l->gi]);   /* Read: Report Reference */
        att_send(h, c, d, 3);
        return;
    }
    l->g = G_MAP; h->rmap_n = 0;
    if (!l->map) { lg(h, "%a: no Report Map", c->addr); gatt_mode(h, c); return; }
    uint8_t d[3] = { 0x0a, 0, 0 }; put16(d + 1, l->map);
    att_send(h, c, d, 3);
}
static void gatt_map_done(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    l->mouse_ok = (uint8_t)bt_hid_mouse_parse(h->rmap, (int)h->rmap_n, (struct hid_mouse *)(void *)l->hmouse);
    kb_parse(h->rmap, h->rmap_n, &l->kbm);
    lg(h, "%a: Report Map %u bytes: %s%s", c->addr, h->rmap_n, l->kbm.ok ? "keyboard " : "", l->mouse_ok ? "mouse" : "");
    for (unsigned q = 0; q < l->kbm.n; q++)
        if (l->kbm.r[q].nkeys || l->kbm.r[q].bm_n) lg(h, "%a: keyboard report %u: %s", c->addr, l->kbm.r[q].id, l->kbm.r[q].bm_n ? "key bitmap (NKRO)" : "key array");
    gatt_mode(h, c);
}
static int wants_notify(struct bt_le *l, int i)
{
    if (!l->chr_cccd[i]) return 0;
    if (l->chr_uuid[i] == U_BOOT_KB || l->chr_uuid[i] == U_BOOT_MS) return l->pm != 0;
    return l->chr_uuid[i] == U_REPORT && l->rep_type[i] == 1;
}
static void gatt_next_cccd(struct bt_host *h, struct bt_conn *c)
{
    struct bt_le *l = &c->l;
    for (; l->gi < l->nchr; l->gi++) {
        if (!wants_notify(l, l->gi)) continue;
        uint8_t d[5] = { 0x12, 0, 0, 0x01, 0x00 }; put16(d + 1, l->chr_cccd[l->gi]);   /* Write Request: notifications on */
        att_send(h, c, d, 5);
        return;
    }
    gatt_up(h, c);
}
/* an answer to our request, or an error for it */
static void gatt_rsp(struct bt_host *h, struct bt_conn *c, const uint8_t *p, unsigned n)
{
    struct bt_le *l = &c->l;
    uint8_t op = p[0];
    int err = op == 0x01, notfound = err && n >= 5 && p[4] == 0x0a;
    switch (l->g) {
    case G_MTU:
        if (op == 0x03 && n >= 3) { uint16_t m = le16(p + 1); l->mtu = m < 247 ? (m < 23 ? 23 : m) : 247; }
        l->g = G_SVC;
        { uint8_t d[9] = { 0x06, 1, 0, 0xff, 0xff, 0x00, 0x28, 0x12, 0x18 }; att_send(h, c, d, 9); }   /* Find By Type Value: the HID service */
        return;
    case G_SVC:
        if (op != 0x07 || n < 5) {
            lg(h, "%a has no HID service", c->addr); say(h, "no keyboard or mouse service on that device");
            l->g = G_FAIL; le_disconnect(h, c, 0x13);
            return;
        }
        l->svc_s = le16(p + 1); l->svc_e = le16(p + 3);
        lg(h, "%a: HID service %x-%x", c->addr, l->svc_s, l->svc_e);
        l->g = G_CHR; l->cursor = l->svc_s;
        att_req3(h, c, 0x08, l->cursor, l->svc_e, 0x2803);
        return;
    case G_CHR:
        if (op == 0x09 && n >= 2) {
            unsigned el = p[1];
            uint16_t last = l->cursor;
            if (el == 7 || el == 21) for (unsigned i = 2; i + el <= n; i += el) {
                uint16_t decl = le16(p + i), vh = le16(p + i + 3);
                uint16_t uu = el == 7 ? le16(p + i + 5) : le16(p + i + 5 + 12);   /* a 128-bit UUID: its 16-bit part (the base UUID) */
                if (l->nchr < BT_LE_CHARS) { l->chr_vh[l->nchr] = vh; l->chr_uuid[l->nchr] = uu; l->chr_cccd[l->nchr] = 0; l->chr_ref[l->nchr] = 0; l->rep_id[l->nchr] = 0; l->rep_type[l->nchr] = 0; l->nchr++; }
                last = decl;
            }
            if (last >= l->cursor && last < l->svc_e && el) { l->cursor = (uint16_t)(last + 1); att_req3(h, c, 0x08, l->cursor, l->svc_e, 0x2803); return; }
        } else if (!notfound && err) lg(h, "%a: characteristics: error %u", c->addr, n >= 5 ? p[4] : 0);
        l->g = G_DESC; l->cursor = l->svc_s;
        att_req3(h, c, 0x04, l->cursor, l->svc_e, 0);
        return;
    case G_DESC:
        if (op == 0x05 && n >= 2) {
            unsigned el = p[1] == 1 ? 4 : 18;
            uint16_t last = l->cursor;
            for (unsigned i = 2; i + el <= n; i += el) {
                uint16_t hd = le16(p + i), uu = el == 4 ? le16(p + i + 2) : le16(p + i + 2 + 12);
                last = hd;
                if (uu != U_CCCD && uu != U_REPREF) continue;
                int own = -1;
                for (int k = 0; k < l->nchr; k++) if (l->chr_vh[k] < hd && (own < 0 || l->chr_vh[k] > l->chr_vh[own])) own = k;
                if (own < 0) continue;
                if (uu == U_CCCD) l->chr_cccd[own] = hd; else l->chr_ref[own] = hd;
            }
            if (last >= l->cursor && last < l->svc_e) { l->cursor = (uint16_t)(last + 1); att_req3(h, c, 0x04, l->cursor, l->svc_e, 0); return; }
        }
        gatt_choose(h, c);
        return;
    case G_REFS:
        if (op == 0x0b && n >= 3 && l->gi < l->nchr) { l->rep_id[l->gi] = p[1]; l->rep_type[l->gi] = p[2]; }
        l->gi++;
        gatt_next_ref(h, c);
        return;
    case G_MAP:
        if (op == 0x0b || op == 0x0d) {
            unsigned k = n - 1;
            if (h->rmap_n + k > sizeof h->rmap) k = sizeof h->rmap - h->rmap_n;
            mcpy(h->rmap + h->rmap_n, p + 1, k); h->rmap_n += k;
            if (n - 1 == (unsigned)l->mtu - 1 && h->rmap_n < sizeof h->rmap) {   /* full: there is more (Read Blob) */
                uint8_t d[5] = { 0x0c, 0, 0, 0, 0 }; put16(d + 1, l->map); put16(d + 3, h->rmap_n);
                att_send(h, c, d, 5);
                return;
            }
        }
        gatt_map_done(h, c);
        return;
    case G_CCCD:
        if (err) lg(h, "%a: notifications refused on %x (%u)", c->addr, l->chr_cccd[l->gi], n >= 5 ? p[4] : 0);
        l->gi++;
        gatt_next_cccd(h, c);
        return;
    default:
        return;
    }
}
static void kb_send(struct bt_host *h, struct bt_conn *c, const uint8_t r[8])
{
    mcpy(c->lastkb, r, 8);
    if (h->io->keyboard) h->io->keyboard(h->io->ctx, r);
}
static void le_notify(struct bt_host *h, struct bt_conn *c, uint16_t hd, const uint8_t *v, unsigned n)
{
    struct bt_le *l = &c->l;
    if (l->g != G_UP) return;
    if (l->nlog < 4) {                               /* the first few, as they came: what the device really sends */
        l->nlog++;
        char t[40]; unsigned k = 0;
        for (unsigned j = 0; j < n && j < 12; j++) { t[k++] = "0123456789abcdef"[v[j] >> 4]; t[k++] = "0123456789abcdef"[v[j] & 15]; t[k++] = ' '; }
        t[k] = 0;
        lg(h, "%a: report on %x, %u bytes: %s", c->addr, hd, n, t);
    }
    if (hd && hd == l->kb_in) { if (n >= 8) kb_send(h, c, v); return; }
    if (hd && hd == l->ms_in) {
        if (n >= 3) { l->lastbtn = v[0]; if (h->io->mouse) h->io->mouse(h->io->ctx, (int8_t)v[1], (int8_t)v[2], n >= 4 ? (int8_t)v[3] : 0, v[0]); }
        return;
    }
    int i = 0;
    for (; i < l->nchr && !(l->chr_vh[i] == hd && l->chr_uuid[i] == U_REPORT); i++) ;
    if (i == l->nchr) return;
    uint8_t id = l->rep_id[i];
    const struct bt_kbrep *kr = 0;
    for (unsigned q = 0; q < l->kbm.n; q++) if (l->kbm.r[q].id == id && (l->kbm.r[q].nkeys || l->kbm.r[q].bm_n)) kr = &l->kbm.r[q];
    if (kr) {
        uint8_t r[8];
        mset(r, 0, 8);
        r[0] = kr->has_mod ? (uint8_t)bits(v, n, kr->mod_pos, 8) : c->lastkb[0];
        unsigned nk = 0;
        for (unsigned k = 0; k < kr->nkeys; k++) { uint8_t u = (uint8_t)bits(v, n, kr->keys_pos + 8 * k, 8); if (u && nk < 6) r[2 + nk++] = u; }
        for (unsigned b = 0; b < kr->bm_n; b++) {
            if (!bits(v, n, kr->bm_pos + b, 1)) continue;
            unsigned u = kr->bm_min + b;
            if (u >= 0xe0 && u <= 0xe7) r[0] |= (uint8_t)(1u << (u - 0xe0));
            else if (u && nk < 6) r[2 + nk++] = (uint8_t)u;
        }
        kb_send(h, c, r);
        return;
    }
    if (l->mouse_ok) {
        const struct hid_mouse *m = (const struct hid_mouse *)(const void *)l->hmouse;
        uint8_t r[64]; unsigned k = 0;
        if (m->ids) r[k++] = id;
        for (unsigned j = 0; j < n && k < sizeof r; j++) r[k++] = v[j];
        struct hid_mouse_report o;
        if (bt_hid_mouse_decode(m, r, (int)k, &o)) {
            l->lastbtn = (uint8_t)o.buttons;
            if (h->io->mouse) h->io->mouse(h->io->ctx, o.dx, o.dy, o.wheel, o.buttons);
        }
    }
}
/* ATT from the device: answers to us, notifications, and its own questions (we serve no attributes) */
static void att_in(struct bt_host *h, struct bt_conn *c, const uint8_t *p, unsigned n)
{
    if (!n) return;
    uint8_t op = p[0];
    switch (op) {
    case 0x1b: if (n >= 3) le_notify(h, c, le16(p + 1), p + 3, n - 3); return;                    /* Notification */
    case 0x1d: { uint8_t r = 0x1e; att_send(h, c, &r, 1); if (n >= 3) le_notify(h, c, le16(p + 1), p + 3, n - 3); return; }   /* Indication */
    case 0x02: { uint8_t r[3] = { 0x03, 0, 0 }; put16(r + 1, 247); att_send(h, c, r, 3); return; }   /* its Exchange MTU */
    case 0x01: case 0x03: case 0x05: case 0x07: case 0x09: case 0x0b: case 0x0d: case 0x13:
        gatt_rsp(h, c, p, n);
        return;
    default:
        if (op & 0x40) return;                       /* commands: no answer */
        if (op == 0x1e || op == 0x19 || op == 0x11 || op == 0x0f || op == 0x17) return;   /* other responses */
        {
            uint8_t r[5] = { 0x01, op, 0, 0, 0 };
            put16(r + 2, n >= 3 ? le16(p + 1) : 0);
            r[4] = (op == 0x04 || op == 0x06 || op == 0x08 || op == 0x10) ? 0x0a : (op == 0x0a || op == 0x0c || op == 0x12) ? 0x01 : 0x06;
            att_send(h, c, r, 5);
        }
        return;
    }
}
/* LE signalling: the device asks for other connection parameters */
static void lesig_in(struct bt_host *h, struct bt_conn *c, const uint8_t *p, unsigned n)
{
    if (n < 4) return;
    uint8_t code = p[0], id = p[1];
    unsigned len = le16(p + 2);
    if (len + 4 > n) return;
    if (code == 0x12 && len >= 8) {                  /* Connection Parameter Update Request: yes */
        uint8_t r[6] = { 0x13, id, 2, 0, 0, 0 };
        l2_send(h, c, CID_LESIG, r, 6);
        uint8_t q[14];
        put16(q, c->handle); mcpy(q + 2, p + 4, 8); put16(q + 10, 0); put16(q + 12, 0);
        cmd(h, C_LE_CONN_UPDATE, q, 14);
        lg(h, "%a: connection interval %u-%u (x1.25 ms), latency %u", c->addr, le16(p + 4), le16(p + 6), le16(p + 8));
        return;
    }
    if (code == 0x01 || code == 0x13 || code == 0x15 || code == 0x07) return;   /* replies to nothing of ours */
    uint8_t r[6] = { 0x01, id, 2, 0, 0, 0 };         /* Command Reject: not understood */
    l2_send(h, c, CID_LESIG, r, 6);
}
static void le_l2_in(struct bt_host *h, struct bt_conn *c, uint16_t cid, const uint8_t *d, unsigned n)
{
    if (cid == CID_ATT) att_in(h, c, d, n);
    else if (cid == CID_SMP) smp_in(h, c, d, n);
    else if (cid == CID_LESIG) lesig_in(h, c, d, n);
}

/* ---- LE events -------------------------------------------------------------------- */
static void le_conn_complete(struct bt_host *h, const uint8_t *p, unsigned len, int enhanced)
{
    if (len < 19) return;
    uint8_t st = p[1];
    uint16_t hd = le16(p + 2) & 0x0fff;
    uint8_t ptype = p[5];
    const uint8_t *a = p + 6;
    if (st == 0x02 && h->le_cancelling) { h->le_cancelling = 0; return; }   /* our cancel */
    int was = h->le_init;
    h->le_init = 0;
    h->le_init_until = now(h) + 1000;
    if (st) {
        lg(h, "LE connection failed (%u)", st);
        if (was == 1 && h->pairing) { h->pairing = 0; say(h, "could not connect - is it in pairing mode?"); }
        h->le_init_until = now(h) + 5000;
        return;
    }
    struct bt_conn *c = 0;
    for (int i = 0; i < BT_MAXCONN && !c; i++) if (!h->conn[i].used) c = &h->conn[i];
    if (!c) { uint8_t r[3]; put16(r, hd); r[2] = 0x13; cmd(h, C_DISCONNECT, r, 3); return; }
    mset(c, 0, sizeof *c);
    c->used = 1; c->le = 1; c->out = 1;
    c->handle = hd;
    mcpy(c->addr, a, 6);
    c->remote_io = 0xff;
    c->l.addr_type = ptype & 1;
    c->l.rat = ptype >= 2 ? 1 : ptype;
    mcpy(c->l.ra, a, 6);
    if (enhanced && ptype >= 2 && len >= 31) {        /* resolved: the air had its private address */
        static const uint8_t zero[6];
        if (mcmp(p + 18, zero, 6)) mcpy(c->l.ra, p + 18, 6);
    }
    c->pairing = h->pairing && !mcmp(h->pair_addr, a, 6);
    lg(h, "%a: LE link up (handle %x, address type %u)", c->addr, hd, ptype);
    struct bt_key *k = key_of(h, a);
    if (k && (k->flags & BTK_LE) && !c->pairing) { c->l.rekey = 1; le_start_enc(h, c, k->rand, k->ediv, k->key); }
    else if (c->pairing) smp_start(h, c);
    else { lg(h, "%a: not paired - disconnecting", a); le_disconnect(h, c, 0x13); }
}
static void le_meta(struct bt_host *h, const uint8_t *p, unsigned len)
{
    if (len < 1) return;
    switch (p[0]) {
    case 0x01: le_conn_complete(h, p, len, 0); return;
    case 0x0a: le_conn_complete(h, p, len, 1); return;
    case 0x02: {                                     /* advertising reports, one after another */
        if (len < 2) return;
        unsigned m = p[1], i = 2;
        for (unsigned r = 0; r < m && i + 10 <= len; r++) {
            unsigned dl = p[i + 8];
            if (i + 10 + dl > len) break;
            le_found(h, p + i + 2, p[i + 1], p + i + 9, dl, (int8_t)p[i + 9 + dl]);
            i += 10 + dl;
        }
        return;
    }
    case 0x05: {                                     /* LTK Request: only when we are the peripheral - answer anyway */
        if (len < 13) return;
        uint16_t hd = le16(p + 1) & 0x0fff;
        struct bt_conn *c = conn_of(h, hd);
        struct bt_key *k = c ? key_of(h, c->addr) : 0;
        uint8_t r[18]; put16(r, hd);
        if (k && (k->flags & BTK_LE)) { mcpy(r + 2, k->key, 16); cmd(h, C_LE_LTK_REPLY, r, 18); }
        else cmd(h, C_LE_LTK_NEG, r, 2);
        return;
    }
    default:
        return;
    }
}
/* Command Complete for LE commands outside the start-up */
static void le_cc(struct bt_host *h, uint16_t op, const uint8_t *ret, unsigned n)
{
    if (op == C_LE_ENCRYPT || op == C_LE_RAND) {
        if (!h->aes_busy) return;
        h->aes_busy = 0;
        struct bt_conn *c = conn_of(h, h->aes_handle);
        if (!c || !c->le) return;
        if (!n || ret[0] || n < (op == C_LE_ENCRYPT ? 17u : 9u)) { smp_fail(h, c, 0x08, "the controller's AES failed"); return; }
        smp_crypto(h, c, ret + 1);
        return;
    }
    if (op == C_LE_CREATE_CANCEL && n && ret[0]) { h->le_cancelling = 0; return; }   /* nothing to cancel: it had just connected */
    if ((op >> 10) == 8 && op != h->pending_op && n && ret[0])
        lg(h, "LE command %x: status %u", op, ret[0]);
}
/* Command Status failures of LE commands */
static void le_cstatus(struct bt_host *h, uint16_t op, uint8_t st)
{
    if (op == C_LE_CREATE) {
        int was = h->le_init;
        h->le_init = 0;
        h->le_init_until = now(h) + 5000;
        if (was == 1 && h->pairing) { h->pairing = 0; say(h, "could not reach the device"); }
        lg(h, "LE connect refused (%u)", st);
    } else if (op == C_LE_START_ENC) {
        for (int i = 0; i < BT_MAXCONN; i++) {
            struct bt_conn *c = &h->conn[i];
            if (!c->used || !c->le || c->encrypted) continue;
            if (c->l.smp == SM_ENC) smp_fail(h, c, 0x08, "encryption could not start");
            else if (c->l.rekey) { lg(h, "%a: encryption could not start (%u)", c->addr, st); le_disconnect(h, c, 0x05); }
        }
    }
}
/* a link went down */
static void le_gone(struct bt_host *h, struct bt_conn *c)
{
    if (c->l.lastbtn && h->io->mouse) h->io->mouse(h->io->ctx, 0, 0, 0, 0);
    if (h->aes_busy && h->aes_handle == c->handle) h->aes_busy = 0;
    if (h->passkey_shown && c->pairing) h->passkey_shown = 0;
    h->le_init_until = now(h) + 500;
}
/* timers: a direct connect that gets no answer, pairing timeouts, the accept list kept armed */
static void le_poll(struct bt_host *h)
{
    if (h->le_init == 1 && due(h, h->le_init_until)) {
        lg(h, "LE connect: no answer");
        le_cancel(h);
        h->le_init_until = now(h) + 2000;
        if (h->pairing) { h->pairing = 0; say(h, "the device did not answer - is it in pairing mode?"); }
    }
    for (int i = 0; i < BT_MAXCONN; i++) {
        struct bt_conn *c = &h->conn[i];
        if (!c->used || !c->le) continue;
        uint8_t s = c->l.smp;
        if (s != SM_IDLE && s != SM_DONE && s != SM_FAILED && due(h, c->l.smp_until)) smp_fail(h, c, 0x08, "pairing timed out");
    }
    if (h->le_init == 2 && (h->le_lists_dirty || h->scanning || h->pairing || !le_keys_waiting(h))) le_cancel(h);
    if (h->le_init || h->le_cancelling || h->scanning || h->pairing || !due(h, h->le_init_until)) return;
    if (!le_keys_waiting(h)) return;
    if (h->le_lists_dirty) le_lists_write(h);
    le_create(h, 1, 0, 0);                           /* the controller connects whichever bonded device advertises */
    h->le_init = 2;
}
