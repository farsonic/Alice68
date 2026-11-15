/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68-Status - the kernel's status page (third_party/Emu68/src/alice/alice_status.h) in a
 * Shell: the kernel's build, uptime, what each ARM core does and how busy it is where that is
 * measured, the SoC's temperature (and clock, voltage, throttling with status.sensors=mbox),
 * Emu68's translation cache and the network units' counters.
 *
 *   Alice68-Status          once
 *   Alice68-Status LOOP     every second, until Ctrl-C
 *
 * It finds the page through devicetree.resource (/alice68 "status") and only reads it.
 */
#include "amiga.h"
#include "../third_party/Emu68/src/alice/alice_status.h"

#define VERSION_STR "1.0 (15.11.2026)"
static const char vers[] __attribute__((used)) = "$VER: Alice68-Status " VERSION_STR;

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

static APTR dos; static ULONG out;
static void puts_(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void dec(ULONG v) { char b[12]; int n = 11; b[n] = 0; do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + n); }
static void hex(ULONG v) { char b[9]; for (int i = 0; i < 8; i++) b[i] = "0123456789abcdef"[(v >> ((7 - i) * 4)) & 15]; b[8] = 0; puts_(b); }
static void dec2(ULONG v) { if (v < 10) puts_("0"); dec(v); }
static void frac(ULONG v, ULONG div, ULONG digits)  /* v / div, then the first `digits` decimals (div a power of 10) */
{
    dec(v / div);
    ULONG r = v % div, d = div;
    if (!digits) return;
    puts_(".");
    for (ULONG i = 0; i < digits && d > 1; i++) { d /= 10; dec(r / d); r %= d; }
}
static void bytes(ULONG v)
{
    if (v >= 10u << 20) { dec(v >> 20); puts_(" MB"); }
    else if (v >= 10u << 10) { dec(v >> 10); puts_(" KB"); }
    else { dec(v); puts_(" bytes"); }
}
static void pad(const char *s, int w) { int n = 0; while (s[n]) n++; puts_(s); while (n++ < w) puts_(" "); }
static int brk(void) { return (SetSignal(0, 1u << 12) & (1u << 12)) != 0; }

static inline APTR OpenResource(const char *n) { return (APTR)lvo(SYSBASE, -498, 0, 0, 0, (ULONG)n); }
/* devicetree.resource (Emu68): sfd bias 6 */
static APTR dtb;
static APTR DT_OpenKey(const char *n)              { return (APTR)lvo(dtb, -6, 0, 0, (ULONG)n, 0); }
static void DT_CloseKey(APTR k)                    { lvo(dtb, -12, 0, 0, (ULONG)k, 0); }
static APTR DT_FindProperty(APTR k, const char *n) { return (APTR)lvo(dtb, -24, 0, 0, (ULONG)k, (ULONG)n); }
static const void *DT_GetPropValue(APTR p)         { return (const void *)lvo(dtb, -48, 0, 0, (ULONG)p, 0); }

static ULONG dt_u32(APTR key, const char *name)
{
    APTR p = DT_FindProperty(key, name);
    const UBYTE *v = p ? DT_GetPropValue(p) : 0;
    return v ? (ULONG)v[0] << 24 | (ULONG)v[1] << 16 | (ULONG)v[2] << 8 | v[3] : 0;
}

/* a consistent copy: seq even and the same before and after */
static int snapshot(volatile const struct alice68_status *s, struct alice68_status *c)
{
    for (int tries = 0; tries < 50; tries++) {
        ULONG a = s->seq;
        if (a & 1) { Delay(dos, 1); continue; }     /* the kernel is writing it (a few microseconds) */
        ULONG n = s->size < sizeof *c ? s->size : sizeof *c;
        memset(c, 0, sizeof *c);
        const volatile UBYTE *src = (const volatile UBYTE *)s;
        for (ULONG i = 0; i < n; i++) ((UBYTE *)c)[i] = src[i];
        if (s->seq == a) return 1;
    }
    return 0;
}

static void show(volatile const struct alice68_status *s)
{
    static struct alice68_status c;
    if (!snapshot(s, &c)) { puts_("(the page did not hold still: try again)\n"); return; }
    c.build[sizeof c.build - 1] = 0;
    puts_("Kernel    "); puts_(c.build[0] ? c.build : "(not filled yet: the kernel's CPU1 loop has not run)"); puts_("\n");
    ULONG t = c.uptime_ms / 1000;
    puts_("Uptime    "); dec(t / 3600); puts_(":"); dec2(t / 60 % 60); puts_(":"); dec2(t % 60); puts_("\n");
    for (int i = 0; i < 4; i++) {
        c.core[i].name[sizeof c.core[i].name - 1] = 0;
        puts_("Core "); dec((ULONG)i); puts_("    "); pad(c.core[i].name, 16);
        if (c.core[i].busy == ALICE68_ST_UNKNOWN) puts_("busy: not measured");
        else { puts_("busy: "); frac(c.core[i].busy, 100, 1); puts_(" %"); }
        puts_("\n");
    }
    puts_("SoC       ");
    if (c.temp_mc == ALICE68_ST_UNKNOWN) puts_("temperature not measured");
    else { frac(c.temp_mc, 1000, 1); puts_(" C"); }
    if (c.flags & ALICE68_STF_MBOX_SENSORS) {
        if (!c.sensors_ms) puts_(", clock and voltage not read yet");
        else {
            puts_(", ARM "); dec(c.arm_hz / 1000000); puts_(" MHz, core "); frac(c.core_uv / 1000, 1000, 3); puts_(" V");
            puts_(", throttled $"); hex(c.throttled);
            if (c.throttled & 1) puts_(" (under-voltage now)");
            else if (c.throttled & 4) puts_(" (throttled now)");
            else if (c.throttled & 2) puts_(" (clock capped now)");
        }
    } else puts_(" (clock, voltage: status.sensors=mbox in cmdline.txt)");
    puts_("\n");
    puts_("JIT       cache "); bytes(c.jit_total); puts_(", free "); bytes(c.jit_free);
    puts_(", "); dec(c.jit_units); puts_(" blocks, "); dec(c.jit_translations); puts_(" translated since boot\n");
    static const char *const kinds[] = { "-", "WiFi", "Ethernet" };
    for (int u = 0; u < 2; u++) {
        const struct alice68_status_net *n = &c.net[u];
        puts_("Unit "); dec((ULONG)u); puts_("    ");
        if (!n->kind) { puts_("(none)\n"); continue; }
        puts_(n->kind < 3 ? kinds[n->kind] : "?"); puts_(n->link ? ", link up" : ", no link");
        puts_(", in "); bytes(n->rx_bytes); puts_(" ("); dec(n->rx_pkts); puts_(" frames), out "); bytes(n->tx_bytes);
        puts_(" ("); dec(n->tx_pkts); puts_(" frames)");
        if (n->rx_drop || n->tx_err) { puts_(", "); dec(n->rx_drop); puts_(" dropped, "); dec(n->tx_err); puts_(" errors"); }
        puts_("\n");
    }
    puts_("USB, SD   ");
    if (c.usb_in_bytes == ALICE68_ST_UNKNOWN) puts_("not counted");
    else { puts_("USB in "); bytes(c.usb_in_bytes); puts_(", out "); bytes(c.usb_out_bytes); }
    puts_("\n");
}

static int word_loop(const char *a, ULONG n)
{
    for (ULONG i = 0; i + 4 <= n; i++)
        if ((a[i] | 32) == 'l' && (a[i + 1] | 32) == 'o' && (a[i + 2] | 32) == 'o' && (a[i + 3] | 32) == 'p') return 1;
    return 0;
}

LONG status_main(const char *args, ULONG arglen)
{
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);
    int loop = word_loop(args, arglen);
    dtb = OpenResource("devicetree.resource");
    ULONG addr = 0, ver = 0;
    if (dtb) {
        APTR key = DT_OpenKey("/alice68");
        if (key) { addr = dt_u32(key, "status"); ver = dt_u32(key, "status-version"); DT_CloseKey(key); }
    }
    if (!addr) { puts_("Alice68-Status: no status page (no /alice68 \"status\" in devicetree.resource: an older kernel?)\n"); CloseLibrary(dos); return 10; }
    volatile const struct alice68_status *s = (volatile const struct alice68_status *)addr;
    if (s->magic != ALICE68_STATUS_MAGIC) { puts_("Alice68-Status: the page at $"); hex(addr); puts_(" is not a status page\n"); CloseLibrary(dos); return 10; }
    puts_("Status page at $"); hex(addr); puts_(", version "); dec(s->version);
    if (ver != s->version) { puts_(" (the device tree says "); dec(ver); puts_(")"); }
    puts_("\n");
    for (;;) {
        show(s);
        if (!loop) break;
        for (int k = 0; k < 5; k++) { if (brk()) { puts_("*** Break\n"); CloseLibrary(dos); return 5; } Delay(dos, 10); }
        puts_("\n");
    }
    CloseLibrary(dos);
    return 0;
}
