/* SPDX-License-Identifier: GPL-2.0-only
 * The kernel's Bluetooth host (src/alice/bt/bthost.c) on the PC against a scripted
 * controller: the firmware download and set-up, a paired keyboard reconnecting by
 * itself, a mouse found and paired from a scan (Just Works), a keyboard paired with
 * a passkey, legacy PIN pairing, L2CAP odds and ends, strangers refused. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bthost.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fake UART and clock ----------------------------------------------- */
static uint8_t sent[64][300]; static unsigned sentn[64], nsent, rd;
static uint32_t clock_ms;
static uint8_t kb[8]; static int nkb; static int mdx, mdy, mwh; static unsigned mbt; static int nmouse, nkeys_cb;
static int verbose;
static void io_send(void *c, const uint8_t *p, unsigned n)
{
    (void)c;
    if (nsent == 64 && rd > 0) {                           /* full: what was read goes */
        memmove(sent[0], sent[rd], (nsent - rd) * sizeof sent[0]); memmove(sentn, sentn + rd, (nsent - rd) * sizeof sentn[0]);
        nsent -= rd; rd = 0;
    }
    if (nsent < 64 && n <= 300) { memcpy(sent[nsent], p, n); sentn[nsent++] = n; }
    else { fails++; printf("FAIL: the sent list overflowed\n"); }
}
static void io_baud(void *c, uint32_t b) { (void)c; (void)b; }
static uint32_t io_ms(void *c) { (void)c; return clock_ms; }
static void io_log(void *c, const char *l) { (void)c; if (verbose) printf("  %s\n", l); }
static void io_kb(void *c, const uint8_t r[8]) { (void)c; memcpy(kb, r, 8); nkb++; }
static void io_mouse(void *c, int dx, int dy, int w, unsigned b) { (void)c; mdx = dx; mdy = dy; mwh = w; mbt = b; nmouse++; }
static void io_keys(void *c) { (void)c; nkeys_cb++; }
static int kicks, last_kick;
static void io_kick(void *c, int a) { (void)c; kicks++; last_kick = a; }
static int npad, pad_port[8]; static unsigned pad_dirs[8], pad_fires[8];   /* the last 8 pad calls, newest at npad-1 */
static void io_pad(void *c, int port, unsigned dirs, unsigned fires)
{
    (void)c;
    pad_port[npad % 8] = port; pad_dirs[npad % 8] = dirs; pad_fires[npad % 8] = fires; npad++;
}
static const struct bt_io io = { 0, io_send, io_baud, io_ms, io_log, io_kb, io_mouse, io_keys, io_kick, io_pad };
static struct bt_host H;

static void reset_sent(void) { nsent = rd = 0; }
static int auto_ans;                                    /* after the start-up: each command answered as a controller does */
static void cc0(uint16_t op);
static void cstatus(uint16_t op, uint8_t st);
static void ev(uint8_t code, const uint8_t *p, unsigned n);
/* the next command sent: its parameters, or 0 */
static const uint8_t *next_cmd(uint16_t op, unsigned *plen, int line)
{
    while (rd < nsent && sent[rd][0] != 0x01) rd++;
    if (rd >= nsent) { fails++; printf("FAIL %d: no command sent, wanted %04x\n", line, op); return 0; }
    uint8_t *b = sent[rd++];
    uint16_t got = (uint16_t)(b[1] | b[2] << 8);
    if (got != op) { fails++; printf("FAIL %d: command %04x sent, wanted %04x\n", line, got, op); return 0; }
    if (plen) *plen = b[3];
    if (auto_ans) {
        static const uint16_t complete[] = { 0x040b, 0x040c, 0x040d, 0x040e, 0x042b, 0x042c, 0x0402,
                                             0x200b, 0x200c, 0x200e, 0x2010, 0x2011, 0x2013, 0x201a, 0x201b, 0x2027, 0x2029, 0x202d, 0 };
        int c = 0;
        for (int i = 0; complete[i]; i++) if (complete[i] == op) c = 1;
        if (c) cc0(op); else cstatus(op, 0);
    }
    return b + 4;
}
#define CMD(op) next_cmd((op), 0, __LINE__)
/* the next ACL frame sent: its L2CAP payload and cid */
static const uint8_t *next_acl(uint16_t *cid, unsigned *n, int line)
{
    while (rd < nsent && sent[rd][0] != 0x02) rd++;
    if (rd >= nsent) { fails++; printf("FAIL %d: no ACL frame sent\n", line); return 0; }
    uint8_t *b = sent[rd++];
    *n = (unsigned)(b[5] | b[6] << 8); *cid = (uint16_t)(b[7] | b[8] << 8);
    if (auto_ans) { uint8_t p[5] = { 1, b[1], (uint8_t)(b[2] & 0x0f), 1, 0 }; ev(0x13, p, 5); }   /* the controller sent it */
    return b + 9;
}
#define ACL(cid, n) next_acl((cid), (n), __LINE__)
static void none_left(int line) { while (rd < nsent) { if (sent[rd][0]) { fails++; printf("FAIL %d: unexpected packet type %u (%02x %02x %02x)\n", line, sent[rd][0], sent[rd][1], sent[rd][2], sent[rd][3]); } rd++; } }

static void ev(uint8_t code, const uint8_t *p, unsigned n)
{
    uint8_t b[300];
    b[0] = 0x04; b[1] = code; b[2] = (uint8_t)n; memcpy(b + 3, p, n);
    for (unsigned i = 0; i < n + 3; i++) bt_rx(&H, b + i, 1);           /* byte by byte, as from a UART */
}
static void cc(uint16_t op, const uint8_t *ret, unsigned n)
{
    uint8_t p[260]; p[0] = 1; p[1] = (uint8_t)op; p[2] = (uint8_t)(op >> 8);
    memcpy(p + 3, ret, n); ev(0x0e, p, n + 3);
}
static void cc0(uint16_t op) { uint8_t z = 0; cc(op, &z, 1); }
static void cstatus(uint16_t op, uint8_t st) { uint8_t p[4] = { st, 1, (uint8_t)op, (uint8_t)(op >> 8) }; ev(0x0f, p, 4); }
static void acl_in(uint16_t handle, uint16_t cid, const uint8_t *d, unsigned n, int split)
{
    uint8_t b[400];
    unsigned L = n + 4;
    b[0] = 0x02; b[1] = (uint8_t)handle; b[2] = (uint8_t)((handle >> 8) | 0x20);
    if (split && L > 6) {                                   /* in two pieces, to check the reassembly */
        unsigned first = 6;
        b[3] = (uint8_t)first; b[4] = 0;
        b[5] = (uint8_t)n; b[6] = (uint8_t)(n >> 8); b[7] = (uint8_t)cid; b[8] = (uint8_t)(cid >> 8);
        memcpy(b + 9, d, first - 4);
        bt_rx(&H, b, 5 + first);
        b[0] = 0x02; b[1] = (uint8_t)handle; b[2] = (uint8_t)((handle >> 8) | 0x10);
        b[3] = (uint8_t)(L - first); b[4] = 0;
        memcpy(b + 5, d + first - 4, L - first);
        bt_rx(&H, b, 5 + L - first);
        return;
    }
    b[3] = (uint8_t)L; b[4] = (uint8_t)(L >> 8);
    b[5] = (uint8_t)n; b[6] = (uint8_t)(n >> 8); b[7] = (uint8_t)cid; b[8] = (uint8_t)(cid >> 8);
    memcpy(b + 9, d, n);
    bt_rx(&H, b, 9 + n);
}
static void sig_in(uint16_t handle, uint8_t code, uint8_t id, const uint8_t *d, unsigned n)
{
    uint8_t b[64]; b[0] = code; b[1] = id; b[2] = (uint8_t)n; b[3] = 0; memcpy(b + 4, d, n);
    acl_in(handle, 1, b, n + 4, 0);
}
/* the next signalling command sent: code, id, data */
static const uint8_t *next_sig(uint8_t code, uint8_t *id, int line)
{
    uint16_t cid; unsigned n;
    const uint8_t *f = next_acl(&cid, &n, line);
    if (!f) return 0;
    if (cid != 1 || f[0] != code) { fails++; printf("FAIL %d: signalling %02x on cid %x, wanted %02x\n", line, f[0], cid, code); return 0; }
    if (id) *id = f[1];
    return f + 4;
}
#define SIG(code, id) next_sig((code), (id), __LINE__)
static void completed(uint16_t handle, unsigned n) { uint8_t p[5] = { 1, (uint8_t)handle, (uint8_t)(handle >> 8), (uint8_t)n, 0 }; ev(0x13, p, 5); }

static const uint8_t KB[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };          /* least significant first */
static const uint8_t MOUSE[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
static const uint8_t KB2[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 };
static const uint8_t OLD[6] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36 };
static const uint8_t STRANGER[6] = { 0x41, 0x42, 0x43, 0x44, 0x45, 0x46 };
static const uint8_t KEY[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

/* the channel set-up for a device that opens its own channels (it called us) */
static void device_opens(uint16_t hdl, uint16_t psm, uint16_t rcid, uint16_t *lcid)
{
    uint8_t d[8], id;
    d[0] = (uint8_t)psm; d[1] = (uint8_t)(psm >> 8); d[2] = (uint8_t)rcid; d[3] = (uint8_t)(rcid >> 8);
    sig_in(hdl, 0x02, 7, d, 4);
    const uint8_t *r = SIG(0x03, &id);
    CHECK(r && id == 7 && (r[2] | r[3] << 8) == rcid && !(r[4] | r[5] << 8), "connection response for psm %x", psm);
    *lcid = r ? (uint16_t)(r[0] | r[1] << 8) : 0;
    const uint8_t *cr = SIG(0x04, &id);                    /* our configuration request */
    CHECK(cr && (cr[0] | cr[1] << 8) == rcid, "our configuration request goes to its cid");
    uint8_t ourid = id;
    d[0] = (uint8_t)*lcid; d[1] = (uint8_t)(*lcid >> 8); d[2] = d[3] = 0;
    sig_in(hdl, 0x04, 8, d, 4);                            /* its request */
    const uint8_t *rs = SIG(0x05, &id);
    CHECK(rs && id == 8 && (rs[0] | rs[1] << 8) == rcid && !(rs[4] | rs[5] << 8), "we accept its configuration");
    d[0] = (uint8_t)*lcid; d[1] = (uint8_t)(*lcid >> 8); d[2] = d[3] = 0; d[4] = d[5] = 0;
    sig_in(hdl, 0x05, ourid, d, 6);                        /* its answer to ours */
}

static void startup(int with_fw)
{
    static const uint8_t fw[] = { 0x4c, 0xfc, 0x05, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,       /* WRITE_RAM */
                                  0x4c, 0xfc, 0x02, 0x11, 0x22,
                                  0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };            /* LAUNCH_RAM */
    reset_sent();
    auto_ans = 0;
    bt_init(&H, &io, with_fw ? fw : 0, with_fw ? sizeof fw : 0);
    CHECK(H.state == BT_FW, "starts in firmware state");
    CMD(0x0c03); cc0(0x0c03);
    CHECK(nsent == rd, "waits 100 ms after the reset");
    clock_ms += 110; bt_poll(&H);
    if (with_fw) {
        uint8_t rv[9] = { 0, 6, 0x00, 0x00, 6, 0x0f, 0x00, 0x19, 0x61 };
        CMD(0x1001); cc(0x1001, rv, 9);                    /* the ROM: which chip */
        uint8_t nm[249] = { 0 }; memcpy(nm + 1, "BCM4345C0", 9);
        CMD(0x0c14); cc(0x0c14, nm, 249);
        CMD(0xfc2e); cc0(0xfc2e);
        CHECK(nsent == rd, "waits 50 ms before the records");
        clock_ms += 60; bt_poll(&H);
        unsigned n;
        const uint8_t *p = next_cmd(0xfc4c, &n, __LINE__);
        CHECK(p && n == 5 && p[0] == 0xaa && p[4] == 0xee, "first record as in the file");
        cc0(0xfc4c);
        p = next_cmd(0xfc4c, &n, __LINE__); CHECK(p && n == 2, "second record"); cc0(0xfc4c);
        CMD(0xfc4e); cc0(0xfc4e);
        CHECK(nsent == rd, "waits 250 ms after the launch");
        clock_ms += 300; bt_poll(&H);
        CMD(0x0c03);
        if (with_fw == 2) {                                /* the new firmware misses the first reset, twice */
            kicks = 0;
            clock_ms += 3100; bt_poll(&H);
            CHECK(kicks == 1 && last_kick == 1 && H.state == BT_FW, "unanswered reset: the line unstuck (%d)", kicks);
            CMD(0x0c03);
            clock_ms += 2100; bt_poll(&H);
            CHECK(kicks == 2 && last_kick == 2, "and again, the second way");
            CMD(0x0c03);
        }
        cc0(0x0c03);
        CHECK(nsent == rd && H.state == BT_FW, "waits 100 ms after the reset");
        clock_ms += 110; bt_poll(&H);
    }
    CHECK(H.state == BT_INIT, "set-up after the firmware");
    uint8_t ver[9] = { 0, 7, 0x22, 0x01, 0x0f, 0x00, 0x09, 0x41, 0x61 };
    CMD(0x1001); cc(0x1001, ver, 9);
    uint8_t ba[7] = { 0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6 };
    CMD(0x1009); cc(0x1009, ba, 7);
    uint8_t bs[8] = { 0, 0xfd, 0x03, 0x40, 0x08, 0x00, 0x00, 0x00 };   /* 1021 bytes, 8 buffers */
    CMD(0x1005); cc(0x1005, bs, 8);
    const uint8_t *m = CMD(0x0c01); CHECK(m && (m[6] & 0x80) == 0x80 && (m[7] & 0x01), "event mask has the SSP events"); cc0(0x0c01);
    m = CMD(0x0c56); CHECK(m && m[0] == 1, "simple pairing on"); cc0(0x0c56);
    m = CMD(0x0c45); CHECK(m && m[0] == 2, "inquiry with EIR"); cc0(0x0c45);
    m = CMD(0x0c24); CHECK(m && m[0] == 0x04 && m[1] == 0x01, "class: desktop computer"); cc0(0x0c24);
    m = CMD(0x0c13); CHECK(m && !strcmp((const char *)m, "Alice68"), "name Alice68"); cc0(0x0c13);
    CMD(0x0c18); cc0(0x0c18);
    CMD(0x080f); cc0(0x080f);
    m = CMD(0x0c6d); CHECK(m && m[0] == 1, "LE host support on"); cc0(0x0c6d);
    uint8_t lbs[4] = { 0, 27, 0, 4 };                      /* LE buffers: 27 bytes, 4 of them */
    CMD(0x2002); cc(0x2002, lbs, 4);
    m = CMD(0x2001); CHECK(m && m[0] == 0x1f, "LE events: connections, adverts, LTK requests"); cc0(0x2001);
    m = CMD(0x0c1a); CHECK(m && m[0] == 2, "page scan only"); cc0(0x0c1a);
    CHECK(H.state == BT_READY && H.bdaddr[0] == 0xa1 && H.acl_free == 8, "ready: state %d addr %02x buffers %u", H.state, H.bdaddr[0], H.acl_free);
    auto_ans = 1;
}


/* ---- a test-only AES-128 (FIPS-197), for the LE Encrypt answers and the expected SMP values ---- */
static uint8_t sbox[256];
static uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }
static void sbox_init(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));          /* p * 3 */
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);   /* q / 3 */
        if (q & 0x80) q ^= 0x09;
        sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
}
static void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])   /* most significant octet first */
{
    uint8_t w[176], s[16], t[16], rc = 1;
    memcpy(w, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t a[4] = { w[i - 4], w[i - 3], w[i - 2], w[i - 1] };
        if (i % 16 == 0) { uint8_t x = a[0]; a[0] = (uint8_t)(sbox[a[1]] ^ rc); a[1] = sbox[a[2]]; a[2] = sbox[a[3]]; a[3] = sbox[x]; rc = xt(rc); }
        for (int j = 0; j < 4; j++) w[i + j] = w[i - 16 + j] ^ a[j];
    }
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ w[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) s[k + 4 * c] = t[k + 4 * ((c + k) % 4)];
        if (r < 10) for (int c = 0; c < 4; c++) {
            uint8_t *m = s + 4 * c, a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
            m[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
            m[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
            m[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
            m[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
        }
        for (int i = 0; i < 16; i++) s[i] ^= w[16 * r + i];
    }
    memcpy(out, s, 16);
}
static void rev16(uint8_t *d, const uint8_t *s) { for (int i = 0; i < 16; i++) d[i] = s[15 - i]; }
static void e_le(const uint8_t k[16], const uint8_t p[16], uint8_t o[16])   /* as SMP and HCI write them: least significant first */
{
    uint8_t K[16], P[16], O[16];
    rev16(K, k); rev16(P, p); aes128(K, P, O); rev16(o, O);
}
static void c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7], uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t o[16])
{
    uint8_t p1[16], p2[16], x[16];
    p1[0] = iat; p1[1] = rat; memcpy(p1 + 2, preq, 7); memcpy(p1 + 9, pres, 7);
    for (int i = 0; i < 16; i++) p1[i] ^= r[i];
    e_le(k, p1, x);
    memcpy(p2, ra, 6); memcpy(p2 + 6, ia, 6); memset(p2 + 12, 0, 4);
    for (int i = 0; i < 16; i++) p2[i] ^= x[i];
    e_le(k, p2, o);
}
static void s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t o[16])
{
    uint8_t r[16]; memcpy(r, r2, 8); memcpy(r + 8, r1, 8); e_le(k, r, o);
}
static void hexle(uint8_t *d, const char *hex, int n)          /* a number written most significant first -> least first */
{
    for (int i = 0; i < n; i++) { unsigned v; sscanf(hex + 2 * (n - 1 - i), "%2x", &v); d[i] = (uint8_t)v; }
}
/* the controller's LE Encrypt and LE Rand: every one waiting is answered */
static uint8_t rand_next = 0x30;
static uint8_t last_rand[64]; static int nrand;
static void crypto_run(void)
{
    for (;;) {
        unsigned i;
        for (i = rd; i < nsent; i++) {
            uint16_t op = (uint16_t)(sent[i][1] | sent[i][2] << 8);
            if (sent[i][0] == 0x01 && (op == 0x2017 || op == 0x2018)) break;
        }
        if (i >= nsent) return;
        uint8_t *b = sent[i];
        uint16_t op = (uint16_t)(b[1] | b[2] << 8);
        b[0] = 0;                                           /* taken: the readers skip it */
        if (op == 0x2017) { uint8_t o[17]; o[0] = 0; e_le(b + 4, b + 20, o + 1); cc(0x2017, o, 17); }
        else { uint8_t o[9]; o[0] = 0; for (int k = 0; k < 8; k++) { o[1 + k] = rand_next++; if (nrand < 64) last_rand[nrand++] = o[1 + k]; } cc(0x2018, o, 9); }
    }
}
/* the next ACL frame, on an LE fixed channel */
static const uint8_t *le_acl(uint16_t want, unsigned *n, int line)
{
    uint16_t cid; unsigned k;
    while (rd < nsent && sent[rd][0] != 0x02) rd++;
    if (rd >= nsent) { fails++; printf("FAIL %d: no ACL frame on cid %x\n", line, want); return 0; }
    const uint8_t *f = next_acl(&cid, &k, line);
    if (cid != want) { fails++; printf("FAIL %d: ACL on cid %x (%02x), wanted cid %x\n", line, cid, f[0], want); return 0; }
    if (n) *n = k;
    return f;
}
#define LEACL(cid, n) le_acl((cid), (n), __LINE__)
static void drain(void)                                    /* whatever was sent: commands answered, frames dropped */
{
    while (rd < nsent) {
        if (sent[rd][0] == 0x01) next_cmd((uint16_t)(sent[rd][1] | sent[rd][2] << 8), 0, __LINE__);
        else rd++;
    }
}
static void le_ev(const uint8_t *p, unsigned n) { ev(0x3e, p, n); }
static void le_conn(uint16_t hd, uint8_t ptype, const uint8_t a[6])
{
    uint8_t p[19] = { 0x01, 0, (uint8_t)hd, (uint8_t)(hd >> 8), 0, ptype };
    memcpy(p + 6, a, 6); p[12] = 12; p[14] = 0; p[16] = 0xf4; p[17] = 1; p[18] = 0;
    le_ev(p, 19);
}
static void le_adv(const uint8_t a[6], uint8_t atype, uint16_t appearance, int hid_uuid, const char *name)
{
    uint8_t p[64] = { 0x02, 1, 0x00, atype }; unsigned i = 4;
    memcpy(p + i, a, 6); i += 6;
    unsigned dl_at = i++, d0 = i;
    if (appearance) { p[i++] = 3; p[i++] = 0x19; p[i++] = (uint8_t)appearance; p[i++] = (uint8_t)(appearance >> 8); }
    if (hid_uuid) { p[i++] = 3; p[i++] = 0x03; p[i++] = 0x12; p[i++] = 0x18; }
    unsigned nl = (unsigned)strlen(name); p[i++] = (uint8_t)(nl + 1); p[i++] = 0x09; memcpy(p + i, name, nl); i += nl;
    p[dl_at] = (uint8_t)(i - d0);
    p[i++] = (uint8_t)-55;
    le_ev(p, i);
}
static void att_in(uint16_t hd, const uint8_t *d, unsigned n) { acl_in(hd, 4, d, n, 0); }
static void smp_in(uint16_t hd, const uint8_t *d, unsigned n) { acl_in(hd, 6, d, n, 0); }
/* SMP legacy pairing as the device; the host's passkey becomes the TK when asked; stk: what the host must use */
static void le_pair_device(uint16_t hd, const uint8_t a[6], uint8_t atype, uint8_t io_cap, uint8_t auth, uint8_t rdist, int use_host_passkey, uint8_t stk[16])
{
    unsigned n;
    const uint8_t *q = LEACL(6, &n);
    uint8_t preq[7] = { 0 };
    CHECK(q && n == 7 && q[0] == 0x01 && q[1] == 0x00 && q[4] == 16 && q[6] == 0x03, "pairing request: DisplayOnly, bonding, its LTK and identity");
    if (q) memcpy(preq, q, 7);
    uint8_t pres[7] = { 0x02, io_cap, 0, auth, 16, 0, rdist };
    nrand = 0;
    smp_in(hd, pres, 7);
    crypto_run();
    uint8_t tk[16]; memset(tk, 0, 16);
    if (use_host_passkey) { CHECK(H.passkey_shown, "a passkey shown"); tk[0] = (uint8_t)H.passkey; tk[1] = (uint8_t)(H.passkey >> 8); tk[2] = (uint8_t)(H.passkey >> 16); }
    uint8_t mrand[16]; memcpy(mrand, last_rand + (use_host_passkey ? 8 : 0), 16);
    uint8_t want[16]; c1(tk, mrand, preq, pres, 0, H.bdaddr, atype, a, want);
    q = LEACL(6, &n);
    CHECK(q && n == 17 && q[0] == 0x03 && !memcmp(q + 1, want, 16), "the host's confirm is c1(TK, Mrand, ...)");
    uint8_t srand[16]; for (int i = 0; i < 16; i++) srand[i] = (uint8_t)(0xa0 + i);
    uint8_t sc[17]; sc[0] = 0x03; c1(tk, srand, preq, pres, 0, H.bdaddr, atype, a, sc + 1);
    smp_in(hd, sc, 17);
    q = LEACL(6, &n);
    CHECK(q && n == 17 && q[0] == 0x04 && !memcmp(q + 1, mrand, 16), "then its random");
    uint8_t sr[17]; sr[0] = 0x04; memcpy(sr + 1, srand, 16);
    smp_in(hd, sr, 17);
    crypto_run();
    s1(tk, srand, mrand, stk);
    const uint8_t *e = CMD(0x2019);
    static const uint8_t z[10];
    CHECK(e && (e[0] | e[1] << 8) == hd && !memcmp(e + 2, z, 10) && !memcmp(e + 12, stk, 16), "encryption with the STK = s1(TK, Srand, Mrand)");
    uint8_t ec[4] = { 0, (uint8_t)hd, (uint8_t)(hd >> 8), 1 };
    ev(0x08, ec, 4);
}
static const uint8_t LTK2[16] = { 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60 };
static const uint8_t RAND2[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t IRK2[16] = { 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0 };
static void le_give_keys(uint16_t hd, const uint8_t a[6], uint8_t atype, int with_id)
{
    uint8_t d[17]; d[0] = 0x06; memcpy(d + 1, LTK2, 16); smp_in(hd, d, 17);
    uint8_t m[11]; m[0] = 0x07; m[1] = 0x34; m[2] = 0x12; memcpy(m + 3, RAND2, 8); smp_in(hd, m, 11);
    if (with_id) {
        d[0] = 0x08; memcpy(d + 1, IRK2, 16); smp_in(hd, d, 17);
        uint8_t ia[8]; ia[0] = 0x09; ia[1] = atype; memcpy(ia + 2, a, 6); smp_in(hd, ia, 8);
    }
}
/* GATT as a boot mouse: HID service 0x10-0x20, Protocol Mode 0x12, Boot Mouse Input 0x14 (CCCD 0x15) */
static void le_gatt_boot_mouse(uint16_t hd)
{
    unsigned n;
    const uint8_t *q = LEACL(4, &n);
    CHECK(q && q[0] == 0x02, "exchange MTU");
    uint8_t mtu[3] = { 0x03, 23, 0 }; att_in(hd, mtu, 3);
    q = LEACL(4, &n);
    CHECK(q && n == 9 && q[0] == 0x06 && q[5] == 0x00 && q[6] == 0x28 && q[7] == 0x12 && q[8] == 0x18, "find the HID service");
    uint8_t sv[5] = { 0x07, 0x10, 0, 0x20, 0 }; att_in(hd, sv, 5);
    q = LEACL(4, &n);
    CHECK(q && q[0] == 0x08 && q[1] == 0x10 && q[3] == 0x20 && q[5] == 0x03 && q[6] == 0x28, "its characteristics");
    uint8_t ch[16] = { 0x09, 7, 0x11, 0, 0x06, 0x12, 0, 0x4e, 0x2a, 0x13, 0, 0x12, 0x14, 0, 0x33, 0x2a }; att_in(hd, ch, 16);
    q = LEACL(4, &n);
    CHECK(q && q[0] == 0x08 && q[1] == 0x14, "more characteristics from 0x14");
    uint8_t er[5] = { 0x01, 0x08, 0x14, 0, 0x0a }; att_in(hd, er, 5);
    q = LEACL(4, &n);
    CHECK(q && q[0] == 0x04 && q[1] == 0x10, "descriptors");
    uint8_t fi[6] = { 0x05, 1, 0x15, 0, 0x02, 0x29 }; att_in(hd, fi, 6);
    q = LEACL(4, &n);
    CHECK(q && q[0] == 0x04 && q[1] == 0x16, "more descriptors");
    uint8_t er2[5] = { 0x01, 0x04, 0x16, 0, 0x0a }; att_in(hd, er2, 5);
    q = LEACL(4, &n);
    CHECK(q && n == 4 && q[0] == 0x52 && q[1] == 0x12 && q[3] == 0, "protocol mode: boot");
    q = LEACL(4, &n);
    CHECK(q && n == 5 && q[0] == 0x12 && q[1] == 0x15 && q[3] == 1 && q[4] == 0, "boot mouse notifications on");
    uint8_t wr = 0x13; att_in(hd, &wr, 1);
}

int main(int argc, char **argv)
{
    verbose = argc > 1; (void)argv;
    char a[18];
    bt_addr_str(KB, a);
    CHECK(!strcmp(a, "06:05:04:03:02:01"), "address text %s", a);

    /* 1: start with firmware; a paired keyboard calls us and types */
    memset(&H, 0, sizeof H);
    bt_add_key(&H, KB, KEY, 4, BTK_KEYBOARD);
    startup(1);
    /* the start-up call to it: it is asleep, the page times out */
    clock_ms += 400; reset_sent(); bt_poll(&H);
    const uint8_t *p = CMD(0x0405);
    CHECK(p && !memcmp(p, KB, 6), "calls the paired keyboard after the start");
    cstatus(0x0405, 0);
    uint8_t cfail[11] = { 0x04, 0, 0 }; memcpy(cfail + 3, KB, 6); cfail[9] = 1;
    ev(0x03, cfail, 11);                                   /* page timeout */
    /* a key pressed: it calls us */
    reset_sent();
    uint8_t creq[10]; memcpy(creq, KB, 6); creq[6] = 0x40; creq[7] = 0x25; creq[8] = 0x00; creq[9] = 1;
    ev(0x04, creq, 10);
    p = CMD(0x0409); CHECK(p && !memcmp(p, KB, 6) && p[6] == 0, "accepted, as master");
    uint8_t cok[11] = { 0, 0x40, 0x00 }; memcpy(cok + 3, KB, 6); cok[9] = 1;
    ev(0x03, cok, 11);
    p = CMD(0x0411); CHECK(p && p[0] == 0x40, "authentication asked");
    ev(0x17, KB, 6);
    p = CMD(0x040b); CHECK(p && !memcmp(p, KB, 6) && !memcmp(p + 6, KEY, 16), "its link key given");
    uint8_t ac[3] = { 0, 0x40, 0 }; ev(0x06, ac, 3);
    p = CMD(0x0413); CHECK(p && p[2] == 1, "encryption asked");
    uint8_t enc[4] = { 0, 0x40, 0, 1 }; ev(0x08, enc, 4);
    none_left(__LINE__);                                   /* it called: it opens the channels */
    uint16_t lctl, lint;
    device_opens(0x40, 0x11, 0x0050, &lctl);
    device_opens(0x40, 0x13, 0x0051, &lint);
    uint16_t cid; unsigned n;
    const uint8_t *f = ACL(&cid, &n);
    CHECK(f && cid == 0x0050 && n == 1 && f[0] == 0x70, "SET_PROTOCOL boot on the control channel (cid %x n %u)", cid, n);
    CHECK(H.keys[0].flags & BTK_CONNECTED, "the keyboard shows connected");
    {                                                      /* a device asking for ERTM (older Xbox pads): refused, basic mode offered */
        uint8_t cr[15] = { (uint8_t)lctl, (uint8_t)(lctl >> 8), 0, 0, 0x04, 9, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t sid;
        sig_in(0x40, 0x04, 21, cr, 15);
        const uint8_t *rr = SIG(0x05, &sid);
        CHECK(rr && sid == 21 && (rr[4] | rr[5] << 8) == 1 && rr[6] == 0x04 && rr[8] == 0, "ERTM asked: unacceptable, basic mode offered");
        uint8_t cb[4] = { (uint8_t)lctl, (uint8_t)(lctl >> 8), 0, 0 };
        sig_in(0x40, 0x04, 22, cb, 4);
        rr = SIG(0x05, &sid);
        CHECK(rr && sid == 22 && !(rr[4] | rr[5] << 8), "then basic mode accepted");
        reset_sent();
    }
    uint8_t rep[10] = { 0xa1, 0x01, 0x02, 0x00, 0x04, 0, 0, 0, 0, 0 };    /* Shift + a */
    acl_in(0x40, lint, rep, 10, 1);
    CHECK(nkb == 1 && kb[0] == 0x02 && kb[2] == 0x04, "keyboard report through (%d)", nkb);
    completed(0x40, 6);
    uint8_t dis[4] = { 0, 0x40, 0, 0x13 }; ev(0x05, dis, 4);
    CHECK(nkb == 2 && !kb[0] && !kb[2], "keys released at the disconnect");
    CHECK(!(H.keys[0].flags & BTK_CONNECTED), "shows disconnected");

    /* 2: a scan finds a mouse and a keyboard; the mouse pairs with Just Works */
    reset_sent();
    CHECK(!bt_scan(&H), "scan");
    p = CMD(0x0401); CHECK(p && p[0] == 0x33 && p[2] == 0x9e && p[3] == 8, "inquiry, GIAC, 10.24 s");
    cstatus(0x0401, 0);
    p = CMD(0x200b); CHECK(p && p[0] == 1, "an active LE scan beside it"); cc0(0x200b);
    p = CMD(0x200c); CHECK(p && p[0] == 1, "LE scan on"); cc0(0x200c);
    uint8_t eir[15 + 20]; memset(eir, 0, sizeof eir);
    eir[0] = 1; memcpy(eir + 1, MOUSE, 6); eir[7] = 1; eir[9] = 0x80; eir[10] = 0x25; eir[11] = 0x00; eir[14] = (uint8_t)-50;
    eir[15] = 9; eir[16] = 0x09; memcpy(eir + 17, "BT Mouse", 8);
    ev(0x2f, eir, sizeof eir);
    uint8_t ir[15]; ir[0] = 1; memcpy(ir + 1, KB2, 6); ir[7] = 1; ir[8] = 0; ir[9] = 0x40; ir[10] = 0x25; ir[11] = 0; ir[12] = 0x34; ir[13] = 0x12; ir[14] = (uint8_t)-60;
    ev(0x22, ir, 15);                                      /* a keyboard, with RSSI, no name */
    uint8_t other[15]; memcpy(other, ir, 15); memcpy(other + 1, STRANGER, 6); other[9] = 0x0c; other[10] = 0x02; other[11] = 0x5a;   /* a phone */
    ev(0x22, other, 15);
    CHECK(H.heard_other == 1, "the phone heard, not listed");
    {                                                      /* LE: a HID keyboard (service 1812, a name), and a beacon */
        static const uint8_t LEKB[6] = { 0x51, 0x52, 0x53, 0x54, 0x55, 0xd6 };
        uint8_t le[64] = { 0x02, 2 }; unsigned i = 2;
        le[i++] = 0; le[i++] = 1; memcpy(le + i, LEKB, 6); i += 6;
        le[i++] = 13;                                      /* data: 16-bit UUIDs (1812), then the name */
        le[i++] = 3; le[i++] = 0x03; le[i++] = 0x12; le[i++] = 0x18;
        le[i++] = 8; le[i++] = 0x09; memcpy(le + i, "Keys LE", 7); i += 7;
        le[i++] = (uint8_t)-70;
        le[i++] = 0; le[i++] = 0; memcpy(le + i, STRANGER, 6); i += 6;     /* a beacon: flags only */
        le[i++] = 3; le[i++] = 2; le[i++] = 0x01; le[i++] = 0x06; le[i++] = (uint8_t)-80;
        ev(0x3e, le, i);
        CHECK(H.nfound == 3 && H.found[2].le, "an LE keyboard listed (%d)", H.nfound);
        struct bt_found *lf = 0;
        for (int k = 0; k < H.nfound; k++) if (H.found[k].le) lf = &H.found[k];
        CHECK(lf && !strcmp(lf->name, "Keys LE") && (lf->cod & 0x1f00) == 0x500, "marked LE, named: %s", lf ? lf->name : "-");
        CHECK(H.heard_other == 2, "the beacon heard, not listed (%u)", H.heard_other);
        H.nfound--;                                        /* the rest as before */
    }
    CHECK(H.nfound == 2 && !strcmp(H.found[0].name, "BT Mouse") && H.found[1].clkoff == 0x1234, "found %d: '%s'", H.nfound, H.found[0].name);
    uint8_t icomp = 0; ev(0x01, &icomp, 1);
    p = CMD(0x200c); CHECK(p && p[0] == 0, "LE scan off at the end"); cc0(0x200c);
    p = CMD(0x0419); CHECK(p && !memcmp(p, KB2, 6) && p[8] == 0x34 && p[9] == 0x92, "asks the keyboard's name");
    uint8_t rn[7 + 248]; memset(rn, 0, sizeof rn); memcpy(rn + 1, KB2, 6); memcpy(rn + 7, "Slim Keys", 9);
    ev(0x07, rn, sizeof rn);
    CHECK(!strcmp(H.found[1].name, "Slim Keys") && !H.names_pending, "named: %s", H.found[1].name);
    reset_sent();
    CHECK(!bt_pair(&H, MOUSE), "pair the mouse");
    p = CMD(0x0405); CHECK(p && !memcmp(p, MOUSE, 6) && p[12] == 1, "calls the mouse");
    cstatus(0x0405, 0);
    memcpy(cok + 3, MOUSE, 6); cok[1] = 0x41; ev(0x03, cok, 11);
    p = CMD(0x0411); CHECK(p && p[0] == 0x41, "authentication");
    ev(0x17, MOUSE, 6);
    CMD(0x040c);                                           /* no key yet */
    uint8_t iocr[9]; memcpy(iocr, MOUSE, 6); iocr[6] = 3; iocr[7] = 0; iocr[8] = 0x04;
    ev(0x32, iocr, 9);                                     /* the mouse: NoInputNoOutput */
    ev(0x31, MOUSE, 6);
    p = CMD(0x042b); CHECK(p && p[6] == 0 && p[8] == 0x04, "io capability: display only, bonding without MITM (%02x)", p ? p[8] : 0);
    uint8_t ucr[10]; memcpy(ucr, MOUSE, 6); memset(ucr + 6, 0, 4);
    ev(0x33, ucr, 10);
    CMD(0x042c);
    uint8_t spc[7] = { 0 }; memcpy(spc + 1, MOUSE, 6); ev(0x36, spc, 7);
    int kc = nkeys_cb;
    uint8_t lkn[23]; memcpy(lkn, MOUSE, 6); memset(lkn + 6, 0x5a, 16); lkn[22] = 4;
    ev(0x18, lkn, 23);
    CHECK(H.nkeys == 2 && nkeys_cb == kc + 1 && (H.keys[1].flags & BTK_MOUSE) && !strcmp(H.keys[1].name, "BT Mouse"), "the mouse's key kept (%d keys)", H.nkeys);
    ac[1] = 0x41; ev(0x06, ac, 3);
    CMD(0x0413);
    enc[1] = 0x41; ev(0x08, enc, 4);
    uint8_t id;
    const uint8_t *cr = SIG(0x02, &id);                    /* we called: we open the control channel */
    CHECK(cr && (cr[0] | cr[1] << 8) == 0x11, "connection request for HID control");
    uint16_t myctl = cr ? (uint16_t)(cr[2] | cr[3] << 8) : 0;
    uint8_t rsp[8] = { 0x60, 0, (uint8_t)myctl, (uint8_t)(myctl >> 8), 0, 0, 0, 0 };
    sig_in(0x41, 0x03, id, rsp, 8);
    const uint8_t *ourconf = SIG(0x04, &id); CHECK(ourconf && ourconf[0] == 0x60, "configure the control channel");
    uint8_t cfr[6] = { (uint8_t)myctl, (uint8_t)(myctl >> 8), 0, 0, 0, 0 };
    sig_in(0x41, 0x05, id, cfr, 6);
    uint8_t cfq[4] = { (uint8_t)myctl, (uint8_t)(myctl >> 8), 0, 0 };
    sig_in(0x41, 0x04, 9, cfq, 4);
    SIG(0x05, &id);
    cr = SIG(0x02, &id);                                   /* then the interrupt channel */
    CHECK(cr && (cr[0] | cr[1] << 8) == 0x13, "then HID interrupt");
    uint16_t myint = cr ? (uint16_t)(cr[2] | cr[3] << 8) : 0;
    uint8_t rsp2[8] = { 0x61, 0, (uint8_t)myint, (uint8_t)(myint >> 8), 0, 0, 0, 0 };
    sig_in(0x41, 0x03, id, rsp2, 8);
    SIG(0x04, &id);
    cfr[0] = (uint8_t)myint; cfr[1] = (uint8_t)(myint >> 8);
    sig_in(0x41, 0x05, id, cfr, 6);
    cfq[0] = (uint8_t)myint; cfq[1] = (uint8_t)(myint >> 8);
    sig_in(0x41, 0x04, 10, cfq, 4);
    SIG(0x05, &id);
    f = ACL(&cid, &n); CHECK(f && cid == 0x60 && f[0] == 0x70, "boot protocol asked");
    uint8_t mrep[6] = { 0xa1, 0x02, 0x01, 0xfb, 0x05, 0xff };   /* left button, -5, +5, wheel -1 */
    acl_in(0x41, myint, mrep, 6, 0);
    CHECK(nmouse == 1 && mdx == -5 && mdy == 5 && mwh == -1 && mbt == 1, "mouse report: %d %d %d %u", mdx, mdy, mwh, mbt);

    /* 3: a keyboard pairs with a passkey (we display, it types) */
    reset_sent();
    bt_pair(&H, KB2);
    CMD(0x0405); cstatus(0x0405, 0);
    memcpy(cok + 3, KB2, 6); cok[1] = 0x42; ev(0x03, cok, 11);
    CMD(0x0411);
    ev(0x17, KB2, 6); CMD(0x040c);
    ev(0x31, KB2, 6);
    p = CMD(0x042b); CHECK(p && p[8] == 0x05, "a keyboard: MITM asked (%02x)", p ? p[8] : 0);
    uint8_t upn[10]; memcpy(upn, KB2, 6); upn[6] = 0x40; upn[7] = 0xe2; upn[8] = 0x01; upn[9] = 0;   /* 123456 */
    ev(0x3b, upn, 10);
    CHECK(H.passkey == 123456 && strstr(H.status, "123456"), "passkey shown: %s", H.status);
    memcpy(spc + 1, KB2, 6); ev(0x36, spc, 7);
    memcpy(lkn, KB2, 6); ev(0x18, lkn, 23);
    CHECK(H.nkeys == 3 && (H.keys[2].flags & BTK_KEYBOARD) && !strcmp(H.keys[2].name, "Slim Keys"), "keyboard kept");

    /* 4: legacy PIN, L2CAP information and echo, SDP, a stranger, forgetting */
    reset_sent();
    ev(0x16, OLD, 6);
    p = CMD(0x040d); CHECK(p && p[6] == 4 && !memcmp(p + 7, "0000", 4), "PIN 0000");
    memcpy(creq, STRANGER, 6);
    ev(0x04, creq, 10);
    p = CMD(0x040a); CHECK(p && !memcmp(p, STRANGER, 6) && p[6] == 0x0f, "a stranger refused");
    uint8_t it[2] = { 2, 0 };
    sig_in(0x41, 0x0a, 11, it, 2);
    const uint8_t *ir2 = SIG(0x0b, &id); CHECK(ir2 && id == 11 && ir2[0] == 2 && !ir2[2], "extended features answered");
    uint8_t echo[3] = { 'h', 'i', '!' };
    sig_in(0x41, 0x08, 12, echo, 3);
    const uint8_t *er = SIG(0x09, &id); CHECK(er && id == 12 && er[0] == 'h', "echo");
    uint16_t lsdp;
    device_opens(0x41, 0x0001, 0x0070, &lsdp);
    uint8_t sdpq[13] = { 0x06, 0x00, 0x01, 0x00, 0x08, 0x35, 0x03, 0x19, 0x11, 0x24, 0x00, 0x40, 0x00 };
    acl_in(0x41, lsdp, sdpq, 13, 0);
    f = ACL(&cid, &n); CHECK(f && cid == 0x0070 && f[0] == 0x07 && f[2] == 0x01 && f[7] == 0x35 && f[8] == 0, "SDP: an empty answer");
    uint8_t psm9[4] = { 0x09, 0, 0x80, 0 };
    sig_in(0x41, 0x02, 13, psm9, 4);
    const uint8_t *nr = SIG(0x03, &id); CHECK(nr && (nr[4] | nr[5] << 8) == 2, "an unknown PSM refused");
    kc = nkeys_cb;
    CHECK(!bt_forget(&H, MOUSE) && H.nkeys == 2 && nkeys_cb == kc + 1, "mouse forgotten");
    p = CMD(0x0406); CHECK(p && p[0] == 0x41, "and disconnected");


    /* LE 0: the AES and SMP's c1 and s1 against the specifications' examples */
    sbox_init();
    {
        uint8_t k[16], pt[16], o[16], w[16];
        for (int i = 0; i < 16; i++) { k[i] = (uint8_t)i; pt[i] = (uint8_t)(i * 0x11); }
        aes128(k, pt, o); hexle(w, "5ac5b47080b7cdd830047b6ad8e0c469", 16);
        CHECK(!memcmp(o, w, 16), "AES-128 as FIPS-197");
        uint8_t z[16] = { 0 }, r[16], preq[7], pres[7], ia[6], ra[6];
        hexle(r, "5783D52156AD6F0E6388274EC6702EE0", 16); hexle(preq, "07071000000101", 7); hexle(pres, "05000800000302", 7);
        hexle(ia, "A1A2A3A4A5A6", 6); hexle(ra, "B1B2B3B4B5B6", 6);
        c1(z, r, preq, pres, 1, ia, 0, ra, o); hexle(w, "1e1e3fef878988ead2a74dc5bef13b86", 16);
        CHECK(!memcmp(o, w, 16), "c1 as the SMP specification");
        uint8_t r1[16], r2[16];
        hexle(r1, "000F0E0D0C0B0A091122334455667788", 16); hexle(r2, "010203040506070899AABBCCDDEEFF00", 16);
        s1(z, r1, r2, o); hexle(w, "9a1fe1f0e8b0f49b5b4216ae796da062", 16);
        CHECK(!memcmp(o, w, 16), "s1 as the SMP specification");
    }

    {
    /* LE 1: a mouse (random static address) found by a scan, paired Just Works, boot protocol */
    memset(&H, 0, sizeof H);
    startup(0);
    CHECK(H.le_mtu == 27 && H.le_free == 4, "LE buffers of their own");
    static const uint8_t LEMS[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0xc6 };
    reset_sent();
    CHECK(!bt_scan(&H), "scan");
    CMD(0x0401); CMD(0x200b); CMD(0x200c);
    le_adv(LEMS, 1, 0x03c2, 0, "LE Mouse");
    CHECK(H.nfound == 1 && H.found[0].le && H.found[0].addr_type == 1 && !strcmp(H.found[0].name, "LE Mouse"), "the LE mouse found");
    CHECK(!bt_pair(&H, LEMS), "pair it");
    CMD(0x0402); CMD(0x200c);
    const uint8_t *lc = CMD(0x200d);
    CHECK(lc && lc[4] == 0 && lc[5] == 1 && !memcmp(lc + 6, LEMS, 6) && lc[12] == 0, "LE connect to it (random address)");
    le_conn(0x0050, 1, LEMS);
    uint8_t stk[16];
    le_pair_device(0x0050, LEMS, 1, 0x03, 0x01, 0x03, 0, stk);
    int kcb = nkeys_cb;
    le_give_keys(0x0050, LEMS, 1, 1);
    CHECK(H.nkeys == 1 && (H.keys[0].flags & BTK_LE) && (H.keys[0].flags & BTK_MOUSE) && !memcmp(H.keys[0].key, LTK2, 16) &&
          H.keys[0].ediv == 0x1234 && !memcmp(H.keys[0].rand, RAND2, 8) && H.keys[0].has_irk && H.keys[0].addr_type == 1 && nkeys_cb == kcb + 1,
          "bonded: its LTK, EDIV, Rand and IRK kept (%d keys)", H.nkeys);
    le_gatt_boot_mouse(0x0050);
    CHECK(strstr(H.status, "mouse connected") && (H.keys[0].flags & BTK_CONNECTED), "mouse connected: %s", H.status);
    int nm0 = nmouse;
    uint8_t note[7] = { 0x1b, 0x14, 0, 0x01, 5, 0xfb, 1 }; att_in(0x0050, note, 7);
    CHECK(nmouse == nm0 + 1 && mdx == 5 && mdy == -5 && mwh == 1 && mbt == 1, "a notification moves the mouse (%d,%d,%d,%u)", mdx, mdy, mwh, mbt);
    uint8_t cpu[12] = { 0x12, 9, 8, 0, 6, 0, 12, 0, 0, 0, 0xf4, 1 }; acl_in(0x0050, 5, cpu, 12, 0);
    unsigned ln; const uint8_t *q = LEACL(5, &ln);
    CHECK(q && ln == 6 && q[0] == 0x13 && q[1] == 9 && q[4] == 0, "connection parameters accepted");
    q = CMD(0x2013); CHECK(q && q[0] == 0x50 && q[2] == 6 && q[4] == 12, "and asked of the controller");
    uint8_t ask[7] = { 0x10, 1, 0, 0xff, 0xff, 0x00, 0x28 }; att_in(0x0050, ask, 7);
    q = LEACL(4, &ln); CHECK(q && ln == 5 && q[0] == 0x01 && q[1] == 0x10 && q[4] == 0x0a, "its GATT questions: nothing here");
    uint8_t dis[4] = { 0, 0x50, 0, 0x13 }; ev(0x05, dis, 4);
    CHECK(mbt == 0 && nmouse == nm0 + 2 && !(H.keys[0].flags & BTK_CONNECTED), "disconnected: the buttons let go");
    /* it comes back: the accept list (with its IRK) connects it, its stored key encrypts */
    clock_ms += 600; bt_poll(&H);
    CMD(0x2010);
    q = CMD(0x2011); CHECK(q && q[0] == 1 && !memcmp(q + 1, LEMS, 6), "on the accept list");
    q = CMD(0x202d); CHECK(q && q[0] == 0, "resolving off to change it");
    CMD(0x2029);
    q = CMD(0x2027); CHECK(q && q[0] == 1 && !memcmp(q + 1, LEMS, 6) && !memcmp(q + 7, IRK2, 16), "its IRK on the resolving list");
    q = CMD(0x202d); CHECK(q && q[0] == 1, "resolving on");
    q = CMD(0x200d); CHECK(q && q[4] == 1, "connect by the accept list");
    CHECK(H.le_init == 2, "waiting for it");
    le_conn(0x0051, 1, LEMS);
    q = CMD(0x2019);
    CHECK(q && q[0] == 0x51 && !memcmp(q + 2, RAND2, 8) && q[10] == 0x34 && q[11] == 0x12 && !memcmp(q + 12, LTK2, 16), "encrypted with the stored LTK, EDIV, Rand");
    uint8_t ec[4] = { 0, 0x51, 0, 1 }; ev(0x08, ec, 4);
    le_gatt_boot_mouse(0x0051);
    att_in(0x0051, note, 7);
    CHECK(mdx == 5 && (H.keys[0].flags & BTK_CONNECTED), "the mouse works again");
    uint8_t ltkr[13] = { 0x05, 0x51, 0 }; le_ev(ltkr, 13);
    q = CMD(0x201a); CHECK(q && q[0] == 0x51 && !memcmp(q + 2, LTK2, 16), "an LTK request answered from the key");

    /* LE 2: a keyboard (public address) with a passkey, report mode from its Report Map */
    static const uint8_t LEKB3[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };
    reset_sent();
    CHECK(!bt_scan(&H), "scan again");
    CMD(0x0401); CMD(0x200b); CMD(0x200c);
    le_adv(LEKB3, 0, 0x03c1, 1, "LE Keys");
    CHECK(!bt_pair(&H, LEKB3), "pair the keyboard");
    CMD(0x0402); CMD(0x200c);
    lc = CMD(0x200d); CHECK(lc && lc[5] == 0, "LE connect (public address)");
    le_conn(0x0060, 0, LEKB3);
    {
        unsigned n2;
        const uint8_t *pr = LEACL(6, &n2);
        CHECK(pr && pr[3] == 0x05, "keyboard: bonding with MITM");
        rd--;                                              /* le_pair_device reads it again */
    }
    uint8_t stk2[16];
    le_pair_device(0x0060, LEKB3, 0, 0x02, 0x05, 0x01, 1, stk2);
    CHECK(H.passkey < 1000000 && !H.passkey_shown, "the passkey was shown, now gone");
    le_give_keys(0x0060, LEKB3, 0, 0);
    struct bt_key *k3 = 0;
    for (int i = 0; i < H.nkeys; i++) if (!memcmp(H.keys[i].addr, LEKB3, 6)) k3 = &H.keys[i];
    CHECK(k3 && (k3->flags & BTK_LE) && (k3->flags & BTK_KEYBOARD) && !k3->has_irk && k3->addr_type == 0, "the keyboard bonded (no IRK)");
    q = LEACL(4, &ln); uint8_t mtu[3] = { 0x03, 23, 0 }; att_in(0x0060, mtu, 3);
    q = LEACL(4, &ln); uint8_t sv[5] = { 0x07, 0x30, 0, 0x40, 0 }; att_in(0x0060, sv, 5);
    q = LEACL(4, &ln);
    uint8_t ch[16] = { 0x09, 7, 0x31, 0, 0x12, 0x32, 0, 0x4d, 0x2a, 0x35, 0, 0x02, 0x36, 0, 0x4b, 0x2a }; att_in(0x0060, ch, 16);
    q = LEACL(4, &ln); uint8_t er[5] = { 0x01, 0x08, 0x36, 0, 0x0a }; att_in(0x0060, er, 5);
    q = LEACL(4, &ln); uint8_t fi[10] = { 0x05, 1, 0x33, 0, 0x02, 0x29, 0x34, 0, 0x08, 0x29 }; att_in(0x0060, fi, 10);
    q = LEACL(4, &ln); uint8_t er2[5] = { 0x01, 0x04, 0x35, 0, 0x0a }; att_in(0x0060, er2, 5);
    q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x34, "read the Report Reference");
    uint8_t rr[3] = { 0x0b, 1, 1 }; att_in(0x0060, rr, 3);
    q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x36, "read the Report Map");
    static const uint8_t map[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
        0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
        0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0 };
    unsigned at = 0;
    while (q && (q[0] == 0x0a || q[0] == 0x0c)) {
        if (q[0] == 0x0c) CHECK((unsigned)(q[3] | q[4] << 8) == at, "read blob at %u", at);
        unsigned k = sizeof map - at > 22 ? 22 : sizeof map - at;
        uint8_t r[23]; r[0] = q[0] == 0x0a ? 0x0b : 0x0d; memcpy(r + 1, map + at, k); at += k;
        att_in(0x0060, r, k + 1);
        q = LEACL(4, &ln);
    }
    CHECK(at == sizeof map, "the whole Report Map read (%u)", at);
    CHECK(q && ln == 5 && q[0] == 0x12 && q[1] == 0x33, "report notifications on");
    uint8_t wr = 0x13; att_in(0x0060, &wr, 1);
    CHECK(strstr(H.status, "keyboard connected"), "keyboard connected: %s", H.status);
    int nk0 = nkb;
    uint8_t kn[11] = { 0x1b, 0x32, 0, 0x02, 0, 0x04, 0x05, 0, 0, 0, 0 }; att_in(0x0060, kn, 11);
    CHECK(nkb == nk0 + 1 && kb[0] == 0x02 && kb[2] == 0x04 && kb[3] == 0x05, "a report-mode key report as a boot one");
    uint8_t dis2[4] = { 0, 0x60, 0, 0x13 }; ev(0x05, dis2, 4);
    CHECK(nkb == nk0 + 2 && !kb[0] && !kb[2], "disconnected: the keys let go");

    /* LE 2b: a keyboard with boot reports and Protocol Mode that sends on its report characteristics
       anyway, one of them a key bitmap (NKRO): report mode, notifications on all, the bitmap read */
    {
        static const uint8_t LEKB4[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
        clock_ms += 600; bt_poll(&H);
        drain();
        CHECK(!bt_scan(&H), "scan");
        drain();
        le_adv(LEKB4, 0, 0x03c1, 1, "NKRO Keys");
        CHECK(!bt_pair(&H, LEKB4), "pair the NKRO keyboard");
        drain();
        le_conn(0x0050, 0, LEKB4);
        uint8_t stk4[16];
        le_pair_device(0x0050, LEKB4, 0, 0x03, 0x01, 0x01, 0, stk4);       /* it cannot type: Just Works */
        le_give_keys(0x0050, LEKB4, 0, 0);
        q = LEACL(4, &ln); uint8_t m4[3] = { 0x03, 23, 0 }; att_in(0x0050, m4, 3);
        q = LEACL(4, &ln); uint8_t sv4[5] = { 0x07, 0x50, 0, 0x70, 0 }; att_in(0x0050, sv4, 5);
        q = LEACL(4, &ln);
        uint8_t c4a[23] = { 0x09, 7, 0x51, 0, 0x06, 0x52, 0, 0x4e, 0x2a,  0x53, 0, 0x12, 0x54, 0, 0x22, 0x2a,  0x56, 0, 0x12, 0x57, 0, 0x4d, 0x2a };
        att_in(0x0050, c4a, 23);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x08 && q[1] == 0x57, "characteristics from 0x57");
        uint8_t c4b[16] = { 0x09, 7, 0x5a, 0, 0x12, 0x5b, 0, 0x4d, 0x2a,  0x5e, 0, 0x02, 0x5f, 0, 0x4b, 0x2a };
        att_in(0x0050, c4b, 16);
        q = LEACL(4, &ln); uint8_t e4[5] = { 0x01, 0x08, 0x5f, 0, 0x0a }; att_in(0x0050, e4, 5);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x04, "descriptors");
        uint8_t d4[22] = { 0x05, 1, 0x55, 0, 0x02, 0x29,  0x58, 0, 0x02, 0x29,  0x59, 0, 0x08, 0x29,  0x5c, 0, 0x02, 0x29,  0x5d, 0, 0x08, 0x29 };
        att_in(0x0050, d4, 22);
        q = LEACL(4, &ln); uint8_t e5[5] = { 0x01, 0x04, 0x5e, 0, 0x0a }; att_in(0x0050, e5, 5);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x59, "report reference 1");
        uint8_t r1[3] = { 0x0b, 1, 1 }; att_in(0x0050, r1, 3);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x5d, "report reference 2");
        uint8_t r2[3] = { 0x0b, 2, 1 }; att_in(0x0050, r2, 3);
        static const uint8_t map4[] = {
            0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
            0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
            0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
            0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x02, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
            0x95, 0x08, 0x81, 0x02, 0x19, 0x00, 0x29, 0x77, 0x95, 0x78, 0x75, 0x01, 0x81, 0x02, 0xc0 };
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x5f, "read the Report Map");
        unsigned at4 = 0;
        while (q && (q[0] == 0x0a || q[0] == 0x0c)) {
            unsigned k = sizeof map4 - at4 > 22 ? 22 : sizeof map4 - at4;
            uint8_t r[23]; r[0] = q[0] == 0x0a ? 0x0b : 0x0d; memcpy(r + 1, map4 + at4, k); at4 += k;
            att_in(0x0050, r, k + 1);
            q = LEACL(4, &ln);
        }
        CHECK(at4 == sizeof map4, "the whole map (%u)", at4);
        CHECK(q && ln == 4 && q[0] == 0x52 && q[1] == 0x52 && q[3] == 1, "protocol mode: report (the map is understood)");
        q = LEACL(4, &ln); CHECK(q && ln == 5 && q[0] == 0x12 && q[1] == 0x55, "boot keyboard notifications on as well");
        uint8_t w4 = 0x13; att_in(0x0050, &w4, 1);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x12 && q[1] == 0x58, "report 1 notifications on"); att_in(0x0050, &w4, 1);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x12 && q[1] == 0x5c, "report 2 (NKRO) notifications on"); att_in(0x0050, &w4, 1);
        CHECK(strstr(H.status, "keyboard connected"), "NKRO keyboard connected: %s", H.status);
        int nk4 = nkb;
        uint8_t nn[3 + 16] = { 0x1b, 0x5b, 0, 0x02, 0x30 };      /* left shift; usages 04 and 05 in the bitmap */
        att_in(0x0050, nn, sizeof nn);
        CHECK(nkb == nk4 + 1 && kb[0] == 0x02 && kb[2] == 0x04 && kb[3] == 0x05 && !kb[4], "an NKRO report as a boot one (%02x %02x %02x)", kb[0], kb[2], kb[3]);
        uint8_t dis4[4] = { 0, 0x50, 0, 0x13 }; ev(0x05, dis4, 4);
        CHECK(!kb[0] && !kb[2], "NKRO keyboard gone: keys let go");
    }

    /* PAD 1: a paired Classic game pad calls us - laid out as the Fantech EOS Mini in its
       Android/D-input mode: the D-pad as X/Y 0..255 (centre 0x7f/0x80), no hat, 12 buttons, no
       report ID. No SET_PROTOCOL boot; its HID descriptor by SDP (the answer in two parts, with a
       continuation state); its reports to the joystick port; the jump button; let go on disconnect */
    static const uint8_t PADA[6] = { 0x91, 0x92, 0x93, 0x94, 0x95, 0x96 };
    uint16_t padlint = 0;
    {
        static const uint8_t eos[] = {
            0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
            0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02,
            0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
            0xc0 };
        drain();
        CHECK(!bt_add_key(&H, PADA, KEY, 4, BTK_PAD), "a paired pad");
        reset_sent();
        uint8_t pq[10]; memcpy(pq, PADA, 6); pq[6] = 0x08; pq[7] = 0x25; pq[8] = 0x00; pq[9] = 1;   /* class: peripheral, game pad */
        ev(0x04, pq, 10);
        p = CMD(0x0409); CHECK(p && !memcmp(p, PADA, 6), "the pad accepted");
        uint8_t pok[11] = { 0, 0x48, 0x00 }; memcpy(pok + 3, PADA, 6); pok[9] = 1;
        ev(0x03, pok, 11);
        p = CMD(0x0411);
        ev(0x17, PADA, 6);
        p = CMD(0x040b); CHECK(p && !memcmp(p, PADA, 6), "its link key given");
        uint8_t pac[3] = { 0, 0x48, 0 }; ev(0x06, pac, 3);
        p = CMD(0x0413);
        uint8_t penc[4] = { 0, 0x48, 0, 1 }; ev(0x08, penc, 4);
        uint16_t pctl;
        device_opens(0x48, 0x11, 0x0080, &pctl);
        device_opens(0x48, 0x13, 0x0081, &padlint);
        uint8_t id;
        const uint8_t *s = SIG(0x02, &id);                      /* no SET_PROTOCOL: our SDP channel */
        CHECK(s && (s[0] | s[1] << 8) == 0x0001, "a pad: we open SDP, not SET_PROTOCOL boot");
        uint16_t slcid = s ? (uint16_t)(s[2] | s[3] << 8) : 0;
        uint8_t d[8] = { 0x82, 0x00, (uint8_t)slcid, (uint8_t)(slcid >> 8), 0, 0, 0, 0 };
        sig_in(0x48, 0x03, id, d, 8);
        s = SIG(0x04, &id); CHECK(s && (s[0] | s[1] << 8) == 0x0082, "our configuration for the SDP channel");
        uint8_t cr6[6] = { (uint8_t)slcid, (uint8_t)(slcid >> 8), 0, 0, 0, 0 };
        sig_in(0x48, 0x05, id, cr6, 6);
        uint8_t cq4[4] = { (uint8_t)slcid, (uint8_t)(slcid >> 8), 0, 0 };
        sig_in(0x48, 0x04, 9, cq4, 4);
        s = SIG(0x05, &id);
        uint16_t cid; unsigned n;
        const uint8_t *f = ACL(&cid, &n);
        CHECK(f && cid == 0x0082 && f[0] == 0x06 && n >= 18 && f[n - 1] == 0, "SDP: ServiceSearchAttribute, HID, 0x0206 (cid %x n %u)", cid, n);
        /* its record: DES { DES { 0x0206 DES { DES { 0x22, text: the descriptor } } } } */
        uint8_t at[160]; unsigned k = 0, dl = sizeof eos;
        at[k++] = 0x35; at[k++] = (uint8_t)(dl + 15);
        at[k++] = 0x35; at[k++] = (uint8_t)(dl + 13);
        at[k++] = 0x09; at[k++] = 0x02; at[k++] = 0x06;
        at[k++] = 0x35; at[k++] = (uint8_t)(dl + 6);
        at[k++] = 0x35; at[k++] = (uint8_t)(dl + 4);
        at[k++] = 0x08; at[k++] = 0x22; at[k++] = 0x25; at[k++] = (uint8_t)dl;
        memcpy(at + k, eos, dl); k += dl;
        unsigned half = k / 2;
        uint8_t r[120]; unsigned m = 0;
        r[m++] = 0x07; r[m++] = f ? f[1] : 0; r[m++] = f ? f[2] : 0; m += 2;
        r[m++] = (uint8_t)(half >> 8); r[m++] = (uint8_t)half; memcpy(r + m, at, half); m += half;
        r[m++] = 2; r[m++] = 0xab; r[m++] = 0xcd;               /* more to come */
        r[3] = (uint8_t)((m - 5) >> 8); r[4] = (uint8_t)(m - 5);
        acl_in(0x48, slcid, r, m, 1);
        f = ACL(&cid, &n);
        CHECK(f && f[0] == 0x06 && n >= 3 && f[n - 3] == 2 && f[n - 2] == 0xab && f[n - 1] == 0xcd, "SDP: asked again with the continuation state");
        m = 0;
        r[m++] = 0x07; r[m++] = f ? f[1] : 0; r[m++] = f ? f[2] : 0; m += 2;
        r[m++] = (uint8_t)((k - half) >> 8); r[m++] = (uint8_t)(k - half); memcpy(r + m, at + half, k - half); m += k - half;
        r[m++] = 0;
        r[3] = (uint8_t)((m - 5) >> 8); r[4] = (uint8_t)(m - 5);
        int np0 = npad;
        uint8_t early[5] = { 0xa1, 0x00, 0x80, 0x01, 0x00 };
        acl_in(0x48, padlint, early, 5, 0);
        CHECK(npad == np0, "a report before the layout is known: dropped");
        acl_in(0x48, slcid, r, m, 0);
        s = SIG(0x06, &id); CHECK(s && (s[0] | s[1] << 8) == 0x0082, "the SDP channel closed");
        CHECK(strstr(H.status, "game pad connected") && H.conn[0].used, "status: %s", H.status);
        struct bt_key *pk = 0;
        for (int i = 0; i < H.nkeys; i++) if (!memcmp(H.keys[i].addr, PADA, 6)) pk = &H.keys[i];
        CHECK(pk && (pk->flags & BTK_PAD) && (pk->flags & BTK_CONNECTED), "the pad's key: a pad, connected");
        uint8_t rep[5] = { 0xa1, 0x00, 0x7f, 0x01, 0x00 };      /* D-pad left (X 0), button 1 */
        acl_in(0x48, padlint, rep, 5, 0);
        CHECK(npad == np0 + 1 && pad_port[(npad - 1) % 8] == 1 && pad_dirs[(npad - 1) % 8] == PAD_LEFT && pad_fires[(npad - 1) % 8] == 1,
              "left + fire on the joystick port (%d: port %d dirs %x fires %x)", npad - np0, pad_port[(npad - 1) % 8], pad_dirs[(npad - 1) % 8], pad_fires[(npad - 1) % 8]);
        acl_in(0x48, padlint, rep, 5, 0);
        CHECK(npad == np0 + 1, "the same state again: nothing new to send");
        uint8_t rep2[5] = { 0xa1, 0x80, 0xff, 0x06, 0x00 };     /* down, buttons 2 and 3 */
        acl_in(0x48, padlint, rep2, 5, 0);
        CHECK(pad_dirs[(npad - 1) % 8] == PAD_DOWN && pad_fires[(npad - 1) % 8] == 6, "down, fire 2 and fire 3 (%x %x)", pad_dirs[(npad - 1) % 8], pad_fires[(npad - 1) % 8]);
        bt_pad_config(&H, 0, 0, 0, 4);                          /* button 4 jumps */
        uint8_t rep3[5] = { 0xa1, 0x80, 0xff, 0x08, 0x00 };     /* down held, button 4 */
        acl_in(0x48, padlint, rep3, 5, 0);
        CHECK(pad_dirs[(npad - 1) % 8] == PAD_UP && pad_fires[(npad - 1) % 8] == 0, "the jump button pushes up, not down (%x)", pad_dirs[(npad - 1) % 8]);
        bt_pad_config(&H, 3, 1, 2, 0);                          /* buttons swapped by cmdline.txt */
        uint8_t rep4[5] = { 0xa1, 0x80, 0x80, 0x04, 0x00 };     /* button 3 */
        acl_in(0x48, padlint, rep4, 5, 0);
        CHECK(pad_dirs[(npad - 1) % 8] == 0 && pad_fires[(npad - 1) % 8] == 1, "bt.pad.buttons: button 3 fires (%x)", pad_fires[(npad - 1) % 8]);
        bt_pad_config(&H, 0, 0, 0, 0);
    }

    /* PAD 2: an LE game pad (the Xbox Wireless Controller's layout) pairs while the Classic one
       plays: its Report Map, report mode, notifications; it gets the mouse port; both let go */
    {
        static const uint8_t LEPAD[6] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xc6 };
        static const uint8_t xmap[] = {
            0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
            0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75, 0x10, 0x81, 0x02,
            0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
            0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02,
            0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
            0xc0 };
        clock_ms += 600; bt_poll(&H);
        drain();
        CHECK(!bt_scan(&H), "scan");
        drain();
        le_adv(LEPAD, 0, 0x03c4, 1, "Xbox Wireless Controller");
        CHECK(!bt_pair(&H, LEPAD), "pair the LE pad");
        drain();
        le_conn(0x0058, 0, LEPAD);
        uint8_t stk5[16];
        le_pair_device(0x0058, LEPAD, 0, 0x03, 0x01, 0x01, 0, stk5);
        le_give_keys(0x0058, LEPAD, 0, 0);
        q = LEACL(4, &ln); uint8_t m5[3] = { 0x03, 23, 0 }; att_in(0x0058, m5, 3);
        q = LEACL(4, &ln); uint8_t sv5[5] = { 0x07, 0x50, 0, 0x70, 0 }; att_in(0x0058, sv5, 5);
        q = LEACL(4, &ln);
        uint8_t c5[16] = { 0x09, 7, 0x51, 0, 0x12, 0x52, 0, 0x4d, 0x2a,  0x55, 0, 0x02, 0x56, 0, 0x4b, 0x2a };
        att_in(0x0058, c5, 16);
        q = LEACL(4, &ln); uint8_t e6[5] = { 0x01, 0x08, 0x56, 0, 0x0a }; att_in(0x0058, e6, 5);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x04, "descriptors");
        uint8_t d5[10] = { 0x05, 1, 0x53, 0, 0x02, 0x29,  0x54, 0, 0x08, 0x29 };
        att_in(0x0058, d5, 10);
        q = LEACL(4, &ln); uint8_t e7[5] = { 0x01, 0x04, 0x55, 0, 0x0a }; att_in(0x0058, e7, 5);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x54, "its report reference");
        uint8_t rr5[3] = { 0x0b, 1, 1 }; att_in(0x0058, rr5, 3);
        q = LEACL(4, &ln); CHECK(q && q[0] == 0x0a && q[1] == 0x56, "read the Report Map");
        unsigned at5 = 0;
        while (q && (q[0] == 0x0a || q[0] == 0x0c)) {
            unsigned k = sizeof xmap - at5 > 22 ? 22 : sizeof xmap - at5;
            uint8_t r[23]; r[0] = q[0] == 0x0a ? 0x0b : 0x0d; memcpy(r + 1, xmap + at5, k); at5 += k;
            att_in(0x0058, r, k + 1);
            q = LEACL(4, &ln);
        }
        CHECK(q && ln == 5 && q[0] == 0x12 && q[1] == 0x53, "report notifications on");
        uint8_t w5 = 0x13; att_in(0x0058, &w5, 1);
        CHECK(strstr(H.status, "game pad connected"), "LE pad connected: %s", H.status);
        int np0 = npad;
        /* X centre, Y centre, hat 3 (right), buttons A (1) and X (4) */
        uint8_t nt[3 + 7] = { 0x1b, 0x52, 0, 0x00, 0x80, 0x00, 0x80, 0x03, 0x09, 0x00 };
        att_in(0x0058, nt, sizeof nt);
        CHECK(npad == np0 + 1 && pad_port[(npad - 1) % 8] == 0 && pad_dirs[(npad - 1) % 8] == PAD_RIGHT && pad_fires[(npad - 1) % 8] == 5,
              "the second pad in the mouse port: right, A fires, X is fire 3 (port %d dirs %x fires %x)",
              pad_port[(npad - 1) % 8], pad_dirs[(npad - 1) % 8], pad_fires[(npad - 1) % 8]);
        uint8_t dis5[4] = { 0, 0x58, 0, 0x13 }; ev(0x05, dis5, 4);
        CHECK(npad == np0 + 2 && pad_port[(npad - 1) % 8] == 0 && !pad_dirs[(npad - 1) % 8] && !pad_fires[(npad - 1) % 8], "LE pad gone: the mouse port let go");
        uint8_t rep[5] = { 0xa1, 0xff, 0x80, 0x01, 0x00 };     /* the Classic pad: right, fire */
        acl_in(0x48, padlint, rep, 5, 0);
        CHECK(pad_port[(npad - 1) % 8] == 1 && pad_dirs[(npad - 1) % 8] == PAD_RIGHT, "the Classic pad still on the joystick port");
        uint8_t dis6[4] = { 0, 0x48, 0, 0x13 }; ev(0x05, dis6, 4);
        CHECK(pad_port[(npad - 1) % 8] == 1 && !pad_dirs[(npad - 1) % 8] && !pad_fires[(npad - 1) % 8], "Classic pad gone: the joystick port let go");
        for (int i = 0; i < H.nkeys; i++) if (!memcmp(H.keys[i].addr, PADA, 6)) bt_forget(&H, PADA);
        drain();
    }

    /* LE 3: a device that wants Secure Connections stops the pairing */
    static const uint8_t LESC[6] = { 0x81, 0x82, 0x83, 0x84, 0x85, 0x86 };
    clock_ms += 600; bt_poll(&H);                            /* the accept list is armed again: the scan cancels it */
    drain();
    CHECK(!bt_scan(&H), "scan");
    drain();
    le_adv(LESC, 0, 0x03c1, 1, "SC Only");
    CHECK(!bt_pair(&H, LESC), "pair it");
    drain();
    le_conn(0x0070, 0, LESC);
    q = LEACL(6, &ln);
    uint8_t pf[2] = { 0x05, 0x03 }; smp_in(0x0070, pf, 2);
    CHECK(strstr(H.status, "Secure Connections") && !H.pairing, "named: %s", H.status);
    q = CMD(0x0406); CHECK(q && q[0] == 0x70, "and disconnected");
    }

    /* 5b: the new firmware answers only the third reset; never: it fails after three */
    memset(&H, 0, sizeof H);
    startup(2);
    {
        static const uint8_t fw1[] = { 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };
        memset(&H, 0, sizeof H); reset_sent(); auto_ans = 0; kicks = 0;
        bt_init(&H, &io, fw1, sizeof fw1);
        CMD(0x0c03); cc0(0x0c03); clock_ms += 110; bt_poll(&H);
        CMD(0x1001); clock_ms += 1100; bt_poll(&H);          /* a ROM that does not say what it is: on all the same */
        CMD(0xfc2e); cc0(0xfc2e); clock_ms += 60; bt_poll(&H);
        CMD(0xfc4e); cc0(0xfc4e); clock_ms += 300; bt_poll(&H);
        for (int i = 0; i < 5; i++) { CMD(0x0c03); clock_ms += 3100; bt_poll(&H); }
        CHECK(H.state == BT_FAILED && kicks == 4 && strstr(H.why, "new firmware"), "three unanswered resets: %s (%d)", H.why, kicks);
    }

    /* 5c: a firmware file cut short (no Launch_RAM at the end) is not loaded */
    {
        static const uint8_t cut[] = { 0x4c, 0xfc, 0x05, 1, 2, 3, 4, 5, 0x4c, 0xfc, 0x09, 1, 2 };
        memset(&H, 0, sizeof H); reset_sent(); auto_ans = 0;
        bt_init(&H, &io, cut, sizeof cut);
        CHECK(!H.fw, "a cut-short firmware file is not used");
        CMD(0x0c03); cc0(0x0c03); clock_ms += 110; bt_poll(&H);
        CHECK(H.state == BT_INIT, "and the chip starts on its own firmware");
    }

    /* 5: no firmware file: the ROM's; a chip that never answers fails */
    memset(&H, 0, sizeof H);
    startup(0);
    memset(&H, 0, sizeof H); reset_sent(); auto_ans = 0;
    bt_init(&H, &io, 0, 0);
    CMD(0x0c03);
    clock_ms += 4000; bt_poll(&H);
    CHECK(H.state == BT_FAILED && strstr(H.why, "does not answer"), "no answer: %s", H.why);

    printf(fails ? "bt_test FAILED (%d)\n" : "bt_test ALL OK\n", fails);
    return fails != 0;
}
