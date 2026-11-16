/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 LOG - print the Alice68 kernel's USB log and alice68-usb.device's
 * from the pass-through board, plus the port table. For a Shell on Workbench,
 * where the splash's log panels can no longer be seen.
 *   Alice68 LOG                 the last 40 lines of each log
 *   Alice68 LOG LINES=200       the last 200;  ALL: everything
 *   Alice68 LOG MATCH=[BT]      only lines with that text (any case)
 *   Alice68 LOG FOLLOW          then new lines as they come, until Ctrl-C
 * Ctrl-C stops it at any line. */
#include "amiga.h"
#include "../third_party/Emu68/src/frf_usb/xhci/alice68_usbpt.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

static APTR dos; static ULONG out;
static void puts_(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void hex(ULONG v, int digits) { char b[9]; for (int i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> ((digits - 1 - i) * 4)) & 15]; b[digits] = 0; puts_(b); }
static void dec(ULONG v) { char b[12]; int n = 11; b[n] = 0; do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + n); }

static int brk(void) { return (SetSignal(0, 1u << 12) & (1u << 12)) != 0; }      /* Ctrl-C */

/* the options: ALL, LINES=n, MATCH=text, FOLLOW (or -f) */
static ULONG opt_lines = 40;
static int opt_follow;
static char opt_match[64];
static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static int word_is(const char *w, unsigned n, const char *k) { unsigned i = 0; for (; i < n && k[i]; i++) if (up(w[i]) != k[i]) return 0; return i == n && !k[i]; }
static void parse_args(const char *a, ULONG n)
{
    ULONG i = 0;
    while (i < n) {
        while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n')) i++;
        ULONG st = i;
        int q = 0;
        while (i < n && (q || (a[i] != ' ' && a[i] != '\t' && a[i] != '\n'))) { if (a[i] == '"') q = !q; i++; }
        const char *w = a + st;
        unsigned len = (unsigned)(i - st), k = 0;
        while (k < len && w[k] != '=') k++;
        if (!len) continue;
        if (word_is(w, len, "ALL")) opt_lines = 0;
        else if (word_is(w, len, "FOLLOW") || word_is(w, len, "-F")) opt_follow = 1;
        else if (k < len && word_is(w, k, "LINES")) { ULONG v = 0; for (unsigned j = k + 1; j < len && w[j] >= '0' && w[j] <= '9'; j++) v = v * 10 + (ULONG)(w[j] - '0'); opt_lines = v; }
        else if (k < len && word_is(w, k, "MATCH")) {
            unsigned j = k + 1, o = 0;
            if (j < len && w[j] == '"') j++;
            for (; j < len && w[j] != '"' && o < sizeof opt_match - 1; j++) opt_match[o++] = w[j];
            opt_match[o] = 0;
        }
    }
}
static int matches(const char *l, unsigned n)
{
    if (!opt_match[0]) return 1;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = 0;
        while (opt_match[j] && i + j < n && up(l[i + j]) == up(opt_match[j])) j++;
        if (!opt_match[j]) return 1;
    }
    return 0;
}

/* walk the ring's lines from..to: count those that match, or print them past the first skip */
static ULONG lines_of(volatile const char *r, ULONG size, ULONG from, ULONG to, int print, ULONG skip)
{
    if (to - from > size) from = to - size;
    char l[256];
    unsigned n = 0;
    ULONG count = 0;
    for (ULONG i = from; i != to; i++) {
        char c = r[i % size];
        if (c != '\n' && n < sizeof l - 1) { l[n++] = c; continue; }
        if (c != '\n') continue;                       /* a very long line: the rest is dropped */
        if (matches(l, n)) {
            if (print && count >= skip) { l[n++] = '\n'; Write(dos, out, l, (LONG)n); if (brk()) return ~0u; }
            count++;
        }
        n = 0;
    }
    if (n && matches(l, n)) { if (print && count >= skip) { l[n++] = '\n'; Write(dos, out, l, (LONG)n); } count++; }
    return count;
}
/* the last opt_lines lines that match (all with ALL); 1 if Ctrl-C stopped it */
static int ring(volatile const char *r, ULONG size, ULONG wr)
{
    ULONG start = wr > size ? wr - size : 0;
    ULONG total = lines_of(r, size, start, wr, 0, 0);
    ULONG skip = opt_lines && total > opt_lines ? total - opt_lines : 0;
    if (skip) { puts_("("); dec(skip); puts_(" earlier lines left out: LINES=n or ALL for more)\n"); }
    return lines_of(r, size, start, wr, 1, skip) == ~0u;
}

LONG usblog_main(const char *args, ULONG arglen)
{
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);
    parse_args(args, arglen);
    /* every autoconfig board AmigaOS configured */
    APTR eb = OpenLibrary("expansion.library", 36);
    struct ConfigDev *cd = 0;
    if (eb) {
        puts_("Autoconfig boards:\n");
        for (struct ConfigDev *c = FindConfigDev(eb, 0, -1, -1); c; c = FindConfigDev(eb, c, -1, -1)) {
            UWORD man = (UWORD)((c->cd_Rom[4] << 8) | c->cd_Rom[5]);
            puts_("  "); hex(man, 4); puts_("/"); hex(c->cd_Rom[1], 2);
            puts_(" at $"); hex((ULONG)c->cd_BoardAddr, 8); puts_(" size $"); hex(c->cd_BoardSize, 8);
            puts_(c->cd_Flags & 2 ? " (shut up)" : "");
            if (man == PT_MANUFACTURER && c->cd_Rom[1] == PT_PRODUCT) { puts_("  <- the USB pass-through board"); cd = c; }
            puts_("\n");
        }
        CloseLibrary(eb);
    } else puts_("expansion.library would not open\n");

    /* the driver: loaded? why did its last Open fail? does it open now? */
    {
        static const char name[] = "alice68-usb.device";
        static const char *const why[] = { "ok", "no expansion.library", "pass-through board not found",
                                           "board not initialised (magic)", "board/driver version mismatch", "unit is not 0" };
        Forbid();
        UBYTE *dev = FindName((UBYTE *)SYSBASE + EXEC_DEVICELIST, name);
        ULONG diag = dev ? *(ULONG *)(dev + 34) : 0, val = dev ? *(ULONG *)(dev + 38) : 0;
        Permit();
        puts_("\nalice68-usb.device: ");
        if (!dev) puts_("not loaded yet\n");
        else { puts_("loaded, last Open: "); puts_(diag < 6 ? why[diag] : "?"); if (diag >= 3) { puts_(" ($"); hex(val, 8); puts_(")"); } puts_("\n"); }
        static UBYTE io[96];
        memset(io, 0, sizeof io);
        LONG err = OpenDevice("DEVS:USBHardware/alice68-usb.device", 0, io, 0);
        puts_("OpenDevice(DEVS:USBHardware/alice68-usb.device, 0): ");
        if (err == 0) { puts_("OK\n"); CloseDevice(io); }
        else {
            puts_("error "); if (err < 0) { puts_("-"); dec((ULONG)-err); } else dec((ULONG)err);
            puts_(err == -1 ? " (not found / did not load, or Open refused - see above)\n" : "\n");
            Forbid();
            dev = FindName((UBYTE *)SYSBASE + EXEC_DEVICELIST, name);
            diag = dev ? *(ULONG *)(dev + 34) : 0; val = dev ? *(ULONG *)(dev + 38) : 0;
            Permit();
            if (dev) { puts_("  driver says: "); puts_(diag < 6 ? why[diag] : "?"); if (diag >= 3) { puts_(" ($"); hex(val, 8); puts_(")"); } puts_("\n"); }
            else puts_("  the driver is not in the device list: the file did not load (check DEVS:USBHardware/)\n");
        }
    }
    puts_("\n");
    if (!cd) { puts_("Alice68 LOG: no pass-through board - not an Alice68 kernel, or frf_usb=neptune\n"); CloseLibrary(dos); return 10; }
    volatile struct pt_hdr *b = cd->cd_BoardAddr;
    if (b->magic != PT_MAGIC) { puts_("Alice68 LOG: board found, but not initialised by the kernel\n"); CloseLibrary(dos); return 10; }
    puts_("Alice68 USB pass-through board at $"); hex((ULONG)b, 8);
    puts_(", alice68-usb.device "); if (b->amiga_ver) { dec(b->amiga_ver >> 16); puts_("."); dec(b->amiga_ver & 0xffff); } else puts_("not opened yet");
    puts_("\n\nPorts handed to Neptune:\n");
    int any = 0;
    for (int p = 0; p < PT_PORTS; p++) {
        if (!b->port[p].present) continue;
        any = 1;
        static const char *const sp[] = { "?", "full", "low", "high", "super" };
        puts_("  port "); dec((ULONG)p + 1); puts_(": "); hex(b->port[p].vidpid >> 16, 4); puts_(":"); hex(b->port[p].vidpid & 0xffff, 4);
        puts_(", "); puts_(sp[b->port[p].speed < 5 ? b->port[p].speed : 0]); puts_(" speed\n");
    }
    if (!any) puts_("  (none)\n");
    puts_("\n--- the kernel's log ---\n");
    if (ring((volatile const char *)((ULONG)b + PT_OFF_KLOG), PT_KLOG_BYTES, b->klog_wr)) { puts_("*** Break\n"); CloseLibrary(dos); return 5; }
    puts_("\n--- alice68-usb.device ---\n");
    if (ring((volatile const char *)((ULONG)b + PT_OFF_ALOG), PT_ALOG_BYTES, b->alog_wr)) { puts_("*** Break\n"); CloseLibrary(dos); return 5; }
    if (opt_follow) {
        /* like tail -f: both logs, as they grow, until Ctrl-C */
        ULONG kw = b->klog_wr, aw = b->alog_wr;
        puts_("\n--- following (Ctrl-C to stop) ---\n");
        for (;;) {
            if (SetSignal(0, 1u << 12) & (1u << 12)) { puts_("*** Break\n"); break; }
            ULONG k = b->klog_wr, a = b->alog_wr;
            if (k != kw) { lines_of((volatile const char *)((ULONG)b + PT_OFF_KLOG), PT_KLOG_BYTES, kw, k, 1, 0); kw = k; }
            if (a != aw) { lines_of((volatile const char *)((ULONG)b + PT_OFF_ALOG), PT_ALOG_BYTES, aw, a, 1, 0); aw = a; }
            Delay(dos, 10);
        }
    }
    CloseLibrary(dos);
    return 0;
}
