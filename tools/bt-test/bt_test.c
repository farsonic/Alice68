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
static void io_send(void *c, const uint8_t *p, unsigned n) { (void)c; if (nsent < 64 && n <= 300) { memcpy(sent[nsent], p, n); sentn[nsent++] = n; } }
static void io_baud(void *c, uint32_t b) { (void)c; (void)b; }
static uint32_t io_ms(void *c) { (void)c; return clock_ms; }
static void io_log(void *c, const char *l) { (void)c; if (verbose) printf("  %s\n", l); }
static void io_kb(void *c, const uint8_t r[8]) { (void)c; memcpy(kb, r, 8); nkb++; }
static void io_mouse(void *c, int dx, int dy, int w, unsigned b) { (void)c; mdx = dx; mdy = dy; mwh = w; mbt = b; nmouse++; }
static void io_keys(void *c) { (void)c; nkeys_cb++; }
static int kicks, last_kick;
static void io_kick(void *c, int a) { (void)c; kicks++; last_kick = a; }
static const struct bt_io io = { 0, io_send, io_baud, io_ms, io_log, io_kb, io_mouse, io_keys, io_kick };
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
        static const uint16_t complete[] = { 0x040b, 0x040c, 0x040d, 0x040e, 0x042b, 0x042c, 0x0402, 0 };
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
    m = CMD(0x0c1a); CHECK(m && m[0] == 2, "page scan only"); cc0(0x0c1a);
    CHECK(H.state == BT_READY && H.bdaddr[0] == 0xa1 && H.acl_free == 8, "ready: state %d addr %02x buffers %u", H.state, H.bdaddr[0], H.acl_free);
    auto_ans = 1;
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
    uint8_t eir[15 + 20]; memset(eir, 0, sizeof eir);
    eir[0] = 1; memcpy(eir + 1, MOUSE, 6); eir[7] = 1; eir[9] = 0x80; eir[10] = 0x25; eir[11] = 0x00; eir[14] = (uint8_t)-50;
    eir[15] = 9; eir[16] = 0x09; memcpy(eir + 17, "BT Mouse", 8);
    ev(0x2f, eir, sizeof eir);
    uint8_t ir[15]; ir[0] = 1; memcpy(ir + 1, KB2, 6); ir[7] = 1; ir[8] = 0; ir[9] = 0x40; ir[10] = 0x25; ir[11] = 0; ir[12] = 0x34; ir[13] = 0x12; ir[14] = (uint8_t)-60;
    ev(0x22, ir, 15);                                      /* a keyboard, with RSSI, no name */
    uint8_t other[15]; memcpy(other, ir, 15); memcpy(other + 1, STRANGER, 6); other[9] = 0x0c; other[10] = 0x02; other[11] = 0x5a;   /* a phone */
    ev(0x22, other, 15);
    CHECK(H.nfound == 2 && !strcmp(H.found[0].name, "BT Mouse") && H.found[1].clkoff == 0x1234, "found %d: '%s'", H.nfound, H.found[0].name);
    uint8_t icomp = 0; ev(0x01, &icomp, 1);
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

    /* 5b: the new firmware answers only the third reset; never: it fails after three */
    memset(&H, 0, sizeof H);
    startup(2);
    {
        static const uint8_t fw1[] = { 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };
        memset(&H, 0, sizeof H); reset_sent(); auto_ans = 0; kicks = 0;
        bt_init(&H, &io, fw1, sizeof fw1);
        CMD(0x0c03); cc0(0x0c03); clock_ms += 110; bt_poll(&H);
        CMD(0xfc2e); cc0(0xfc2e); clock_ms += 60; bt_poll(&H);
        CMD(0xfc4e); cc0(0xfc4e); clock_ms += 300; bt_poll(&H);
        for (int i = 0; i < 3; i++) { CMD(0x0c03); clock_ms += 3100; bt_poll(&H); }
        CHECK(H.state == BT_FAILED && kicks == 2 && strstr(H.why, "new firmware"), "three unanswered resets: %s (%d)", H.why, kicks);
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
