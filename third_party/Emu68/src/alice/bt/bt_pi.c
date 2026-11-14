/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 kernel Bluetooth on the Pi 4: the CYW43455's Bluetooth half on UART0
 * (the PL011, GPIO 30-33 in ALT3: CTS, RTS, TXD, RXD, with hardware flow
 * control) and its BT_ON line (the firmware's GPIO expander, line 0), driving
 * bthost.c from CPU1's loop (alice_net_poll). Everything is polled: no
 * interrupts, no waiting - the start-up's pauses are timestamps.
 *
 * The firmware (BCM4345C0.hcd, or BCM4345C5.hcd) comes from AmigaOS like the
 * WiFi's: alice68hdf.device hands over the WIFI folder's files. Paired devices
 * come from cmdline.txt (bt.dev=ADDR:KEY:KIND, written by Alice68-Card), so a
 * keyboard known before reconnects as soon as Bluetooth is up.
 *
 * Keyboards feed the same path as USB ones (frf_native_usb_keyboard_boot_report:
 * Ctrl-Amiga-Amiga and the rest work alike); mice the virtual mouse port and
 * the wheel (pt_mouse_extra) for Alice68-Wheel.
 */
#include <stdint.h>
#include "support.h"
#include "bthost.h"
#include "../alice_net.h"

extern int alice_mbox(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out);
extern const uint8_t *alice_fw_get(const char *const *stems, const char *suffix, uint32_t *len);
extern int alice_fw_offered(void);
extern volatile uint8_t *alice_net_board(void);
extern void alice_net_ring(const char *s, unsigned n);
extern const char *alice_bootargs(void);
extern void frf_post_bt(const char *text, int col);
extern void frf_native_usb_keyboard_boot_report(const uint8_t report[8]) __attribute__((weak));
extern void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);
extern void pt_mouse_extra(int wheel, int pan, unsigned buttons) __attribute__((weak));

#define PERI      0xf2000000u
#define GPIO      (PERI + 0x200000u)
#define UART0     (PERI + 0x201000u)
#define U_DR      0x00
#define U_FR      0x18
#define U_IBRD    0x24
#define U_FBRD    0x28
#define U_LCRH    0x2c
#define U_CR      0x30
#define U_IMSC    0x38
#define U_ICR     0x44
#define FR_RXFE   (1u << 4)
#define FR_TXFF   (1u << 5)
#define FR_BUSY   (1u << 3)

static inline uint32_t rd(uint32_t a) { return __builtin_bswap32(*(volatile uint32_t *)(uintptr_t)a); }
static inline void wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = __builtin_bswap32(v); }
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t ms_now(void) { return (uint32_t)(ticks() / (tick_hz() / 1000u)); }

static struct bt_host bh;
static uint32_t uart_hz = 48000000u;
static int phase;                              /* 0 not started, 1 waiting for firmware, 2 BT_ON low, 3 BT_ON high, 4 running, 7 power off for a retry, 8 failed, 9 off */
static uint32_t phase_t, wait_since;
static char shown[64];
static int shown_col = -1;
static int keys_loaded, uart_done, retried, fail_logged;
static uint32_t rx_total, rx_errors;

static void blog(const char *s)
{
    kprintf("%s\n", s);
    unsigned n = 0;
    while (s[n]) n++;
    if (n > 5 && s[0] == '[') alice_net_ring(s, n), alice_net_ring("\n", 1);
}

/* ---- the UART ------------------------------------------------------------------- */
static void uart_baud(uint32_t baud)
{
    uint64_t t = ticks();
    while ((rd(UART0 + U_FR) & FR_BUSY) && ticks() - t < tick_hz() / 50) ;
    uint32_t cr = rd(UART0 + U_CR);
    wr(UART0 + U_CR, 0);
    uint32_t div = (uint32_t)(((uint64_t)uart_hz * 4u + baud / 2) / baud);       /* 64ths of the divisor */
    wr(UART0 + U_IBRD, div >> 6);
    wr(UART0 + U_FBRD, div & 63u);
    wr(UART0 + U_LCRH, 0x60u | 0x10u);                                          /* 8 bits, FIFOs on */
    wr(UART0 + U_CR, cr ? cr : (1u | (1u << 8) | (1u << 9) | (1u << 14) | (1u << 15)));
}
static void uart_init(void)
{
    uint32_t f = rd(GPIO + 0x0c);                       /* GPFSEL3: GPIO 30-33 to ALT3 (UART0 CTS RTS TXD RXD) */
    f = (f & ~0xfffu) | 07777u;
    wr(GPIO + 0x0c, f);
    uint32_t f1 = rd(GPIO + 0x04);                      /* GPIO 15 an input: UART0's RX comes from GPIO 33 only */
    wr(GPIO + 0x04, f1 & ~(7u << 15));
    uint32_t p1 = rd(GPIO + 0xe8), p2 = rd(GPIO + 0xec);    /* pulls (BCM2711): CTS and RXD up */
    p1 = (p1 & ~(0xfu << 28)) | (1u << 28);
    p2 = (p2 & ~0xfu) | (1u << 2);
    wr(GPIO + 0xe8, p1); wr(GPIO + 0xec, p2);
    uint32_t hz = 0;
    if (alice_mbox(0x00030002u, 2u, 0u, &hz) == 0 && hz > 1000000u) uart_hz = hz;    /* GET_CLOCK_RATE: UART */
    wr(UART0 + U_CR, 0);
    wr(UART0 + U_IMSC, 0);
    wr(UART0 + U_ICR, 0x7ffu);
    uart_baud(115200);
    wr(UART0 + U_CR, 1u | (1u << 8) | (1u << 9) | (1u << 14) | (1u << 15));    /* on, TX, RX, RTS/CTS flow control */
    while (!(rd(UART0 + U_FR) & FR_RXFE)) (void)rd(UART0 + U_DR);              /* whatever was there */
}
/* what goes to the chip waits in a ring and leaves as fast as the UART (and the chip's CTS)
   let it, from every turn of CPU1's loop: the chip may hold CTS for a while after a command,
   and a byte cut off mid-command loses the whole command */
static uint8_t txr[8192];
static unsigned tx_h, tx_t;
static uint32_t tx_moved, tx_warned;
static void tx_drain(void)
{
    while (tx_h != tx_t && !(rd(UART0 + U_FR) & FR_TXFF)) { wr(UART0 + U_DR, txr[tx_h % sizeof txr]); tx_h++; tx_moved = ms_now(); }
    if (tx_h != tx_t && ms_now() - tx_moved > 2000 && !tx_warned) {
        tx_warned = 1;
        blog("[BT] the chip has held CTS for 2 s: it is not taking data");
    }
    if (tx_h == tx_t) tx_warned = 0;
}
static void io_send(void *c, const uint8_t *p, unsigned n)
{
    (void)c;
    if (tx_t - tx_h + n > sizeof txr) { blog("[BT] the send ring is full: a packet dropped"); return; }
    if (tx_h == tx_t) tx_moved = ms_now();
    for (unsigned i = 0; i < n; i++) txr[tx_t++ % sizeof txr] = p[i];
    tx_drain();
}
static void io_baud(void *c, uint32_t b) { (void)c; uart_baud(b); }
static uint32_t io_ms(void *c) { (void)c; return ms_now(); }
static void io_log(void *c, const char *l) { (void)c; blog(l); }
static void io_kb(void *c, const uint8_t r[8]) { (void)c; if (frf_native_usb_keyboard_boot_report) frf_native_usb_keyboard_boot_report(r); }
static void io_mouse(void *c, int dx, int dy, int w, unsigned b)
{
    (void)c;
    aga_virtual_mouse_update(dx, dy, b & 7u);
    if (pt_mouse_extra) pt_mouse_extra(w, 0, b);
}
static void io_keys(void *c) { (void)c; }
static const struct bt_io bio = { 0, io_send, io_baud, io_ms, io_log, io_kb, io_mouse, io_keys };

/* ---- cmdline.txt: bt=off, bt.dev=ADDR:KEY:KIND ----------------------------------------- */
static int word(const char *s, const char *w)
{
    unsigned n = 0;
    while (w[n]) n++;
    for (const char *p = s; *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        unsigned i = 0;
        while (i < n && p[i] == w[i]) i++;
        if (i == n && (!p[n] || p[n] == ' ' || p[n] == '\n')) return 1;
    }
    return 0;
}
int bt_pi_wanted(void) { return !word(alice_bootargs(), "bt=off"); }
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void load_keys(void)
{
    const char *s = alice_bootargs();
    for (const char *p = s; *p; p++) {
        if ((p != s && p[-1] != ' ') || p[0] != 'b' || p[1] != 't' || p[2] != '.' || p[3] != 'd' || p[4] != 'e' || p[5] != 'v' || p[6] != '=') continue;
        const char *q = p + 7;
        uint8_t a[6], k[16];
        int ok = 1;
        for (int i = 0; i < 6 && ok; i++) { int h = hexv(q[2 * i]), l = hexv(q[2 * i + 1]); if (h < 0 || l < 0) ok = 0; else a[5 - i] = (uint8_t)(h << 4 | l); }
        if (!ok || q[12] != ':') continue;
        q += 13;
        for (int i = 0; i < 16 && ok; i++) { int h = hexv(q[2 * i]), l = hexv(q[2 * i + 1]); if (h < 0 || l < 0) ok = 0; else k[i] = (uint8_t)(h << 4 | l); }
        if (!ok) continue;
        uint32_t kind = q[32] == ':' && hexv(q[33]) >= 0 ? (uint32_t)hexv(q[33]) : 0;
        bt_add_key(&bh, a, k, 4, kind << 1 & (BTK_KEYBOARD | BTK_MOUSE));
    }
}

/* ---- the board: what Alice68-Card sees and asks ---------------------------------------- */
static void board_sync(void)
{
    volatile uint8_t *b = alice_net_board();
    if (!b) return;
    volatile struct an_bt *x = (volatile struct an_bt *)(b + AN_OFF_BT);
    if (phase == 4 && x->cmd_seq != x->cmd_done) {
        uint8_t a[6];
        for (int i = 0; i < 6; i++) a[i] = x->cmd_addr[i];
        int r = -1;
        switch (x->cmd) {
        case AN_BT_CMD_SCAN:    r = bt_scan(&bh); break;
        case AN_BT_CMD_PAIR:    r = bt_pair(&bh, a); break;
        case AN_BT_CMD_FORGET:  r = bt_forget(&bh, a); break;
        case AN_BT_CMD_CONNECT: r = bt_connect(&bh, a); break;
        }
        x->cmd_result = (unsigned)r;
        __asm__ volatile("dmb ish" ::: "memory");
        x->cmd_done = x->cmd_seq;
    }
    x->state = phase == 9 ? AN_BT_OFF : phase == 8 ? AN_BT_FAILED : phase < 4 || phase == 7 ? (phase == 1 ? AN_BT_NOFW : AN_BT_STARTING) :
               bh.state == BT_READY ? AN_BT_READY : bh.state == BT_FAILED ? AN_BT_FAILED : AN_BT_STARTING;
    x->scanning = (unsigned)bh.scanning; x->pairing = (unsigned)bh.pairing;
    x->passkey = bh.passkey; x->passkey_shown = (unsigned)bh.passkey_shown;
    x->keys_gen = bh.keys_gen;
    for (int i = 0; i < 6; i++) x->bdaddr[i] = bh.bdaddr[i];
    for (unsigned i = 0; i < sizeof x->msg; i++) { x->msg[i] = bh.status[i]; if (!bh.status[i]) break; }
    x->nkeys = (unsigned)bh.nkeys;
    for (int i = 0; i < bh.nkeys && i < AN_BT_KEYS; i++) {
        volatile struct an_btkey *k = &x->keys[i];
        for (int j = 0; j < 6; j++) k->addr[j] = bh.keys[i].addr[j];
        for (int j = 0; j < 16; j++) k->key[j] = bh.keys[i].key[j];
        k->type = bh.keys[i].type; k->flags = bh.keys[i].flags;
        for (int j = 0; j < 40; j++) { k->name[j] = bh.keys[i].name[j]; if (!bh.keys[i].name[j]) break; }
    }
    x->nfound = (unsigned)bh.nfound;
    for (int i = 0; i < bh.nfound && i < AN_BT_FOUND; i++) {
        volatile struct an_btfound *f = &x->found[i];
        for (int j = 0; j < 6; j++) f->addr[j] = bh.found[i].addr[j];
        f->cod = bh.found[i].cod; f->rssi = bh.found[i].rssi;
        for (int j = 0; j < 40; j++) { f->name[j] = bh.found[i].name[j]; if (!bh.found[i].name[j]) break; }
    }
}

static void post(const char *s, int col)
{
    int same = col == shown_col;
    for (unsigned i = 0; same && i < sizeof shown; i++) { if (shown[i] != s[i]) same = 0; if (!s[i]) break; }
    if (same) return;
    unsigned i = 0;
    for (; s[i] && i + 1 < sizeof shown; i++) shown[i] = s[i];
    shown[i] = 0;
    shown_col = col;
    frf_post_bt(shown, col);
}

/* ---- CPU1's turn ------------------------------------------------------------------- */
void bt_pi_poll(void)
{
    static const char *const stems[] = { "BCM4345C0", "BCM4345C5", "bcm4345c0", 0 };
    uint32_t now = ms_now();
    if (!keys_loaded) { keys_loaded = 1; load_keys(); }
    switch (phase) {
    case 0:
        if (!bt_pi_wanted()) { phase = 9; post("off (bt=off)", 0); blog("[BT] off: bt=off in cmdline.txt"); break; }
        if (!alice_net_board()) return;                 /* AmigaOS is not up yet */
        phase = 1; wait_since = now;
        post("waiting for its firmware", 0);
        /* fall through */
    case 1: {
        uint32_t len = 0;
        const uint8_t *fw = alice_fw_get(stems, ".hcd", &len);
        if (!fw && !(alice_fw_offered() && now - wait_since > 3000) && now - wait_since < 30000) break;
        if (!fw) blog("[BT] no BCM4345C0.hcd in the WIFI folder: the chip's own (older) firmware is used");
        bh.fw = fw; bh.fwlen = fw ? len : 0;
        if (!uart_done) { uart_init(); uart_done = 1; }
        if ((int32_t)(now - phase_t) < 250) break;      /* a mailbox try at most 4 times a second */
        phase_t = now;
        if (alice_mbox(0x00038041u, 128u + 0u, 0u, 0)) {           /* BT_ON low first; the mailbox may be busy */
            if (now - wait_since > 40000) { phase = 8; post("FAILED: BT_ON could not be set", 2); blog("[BT] FAIL: the mailbox would not set BT_ON"); }
            break;
        }
        phase = 2; phase_t = now;
        post("starting", 0);
        break;
    }
    case 2:
        if ((int32_t)(now - phase_t) < 20) break;
        if (alice_mbox(0x00038041u, 128u + 0u, 1u, 0)) { phase_t = now + 230; break; }   /* BT_ON high; again in 250 ms */
        phase = 3; phase_t = now;
        break;
    case 3:
        if ((int32_t)(now - phase_t) < 250) break;      /* the chip boots its ROM */
        while (!(rd(UART0 + U_FR) & FR_RXFE)) (void)rd(UART0 + U_DR);
        phase = 4;
        tx_h = tx_t = 0;
        bt_init(&bh, &bio, bh.fw, bh.fwlen);
        break;
    case 7:                                             /* BT_ON low again, for a fresh start */
        if ((int32_t)(now - phase_t) < 0) break;
        if (alice_mbox(0x00038041u, 128u + 0u, 0u, 0)) { phase_t = now + 250; break; }
        phase = 2; phase_t = now;
        break;
    case 4: {
        tx_drain();
        if (bh.state == BT_FAILED && bh.fw && !retried) {  /* the firmware file did not go in: the chip's own, once */
            retried = 1;
            fail_logged = 0;
            blog("[BT] loading BCM4345C0.hcd failed: trying again with the chip's own firmware");
            bh.fw = 0; bh.fwlen = 0;
            phase = 7; phase_t = now;
            post("starting again (without the firmware file)", 0);
            break;
        }
        uint8_t buf[64];
        for (int rounds = 0; rounds < 8; rounds++) {
            unsigned n = 0;
            while (n < sizeof buf && !(rd(UART0 + U_FR) & FR_RXFE)) {
                uint32_t v = rd(UART0 + U_DR);
                if (v & 0xf00u) rx_errors++;               /* framing, parity, break, overrun */
                buf[n++] = (uint8_t)v;
            }
            rx_total += n;
            if (!n) break;
            bt_rx(&bh, buf, n);
        }
        bt_poll(&bh);
        tx_drain();
        int col = bh.state == BT_READY ? 1 : bh.state == BT_FAILED ? 2 : 0;
        if (bh.state == BT_FAILED && !fail_logged) {
            fail_logged = 1;
            char t[160];
            unsigned k = 0;
            uint32_t vals[7] = { rx_total, rx_errors, tx_h, tx_t - tx_h, rd(UART0 + U_FR), rd(UART0 + U_CR), uart_hz };
            static const char *const parts[7] = { "[BT] UART: ", " bytes from the chip, ", " with errors; ", " sent, ", " waiting; FR ", " CR ", " clock " };
            for (int i = 0; i < 7; i++) {
                for (const char *q = parts[i]; *q && k < 140; ) t[k++] = *q++;
                char d[12]; int n = 0; uint32_t v = vals[i]; int hexv_ = i == 4 || i == 5;
                do { d[n++] = "0123456789abcdef"[v % (hexv_ ? 16u : 10u)]; v /= hexv_ ? 16u : 10u; } while (v && n < 11);
                while (n && k < 150) t[k++] = d[--n];
            }
            t[k] = 0;
            blog(t);
        }
        if (bh.state == BT_FAILED) { char t[64] = "FAILED: "; unsigned k = 8; for (unsigned i = 0; bh.why[i] && k < 63; i++) t[k++] = bh.why[i]; t[k] = 0; post(t, 2); }
        else post(bh.status[0] ? bh.status : "starting", col);
        break;
    }
    default:
        break;
    }
    board_sync();
}
