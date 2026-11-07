/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68-ShowConfig - what this Amiga is made of, as text in a Shell: all that
 * ShowConfig says (processor, Kickstart, chipset, memory, expansion boards) and
 * more: every board's Zorro type, address, size, serial, flags and driver, names
 * for Emu68's and Alice68's boards, and what is live on Alice68's own cards - the
 * USB card's ports and requests, the network card's two units (link, addresses,
 * WiFi, counters, queues), alice68hdf.device's hardfiles and folders - and,
 * on request, the libraries, devices and resources in memory and the kernel log.
 *
 *   Alice68-ShowConfig [SYS] [MEM] [BOARDS] [CARDS] [LIBS] [LOG] [ALL]
 *
 * No words: SYS MEM BOARDS CARDS. ALL: everything. It only reads: nothing is
 * changed, opened on a card or sent to the kernel.
 */
#include <stdarg.h>
#include "amiga.h"
#include "../third_party/Emu68/src/frf_usb/xhci/alice68_usbpt.h"
#include "../third_party/Emu68/src/alice/alice_net.h"
#include "hdf/hdf_dev.h"

#define VERSION_STR "1.0 (07.11.2026)"
static const char vers[] __attribute__((used)) = "$VER: Alice68-ShowConfig " VERSION_STR;

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

/* ---- output ------------------------------------------------------------------ */
static APTR dos;
static ULONG out;
static char obuf[512];
static ULONG on;
static void flush(void) { if (on) Write(dos, out, obuf, (LONG)on); on = 0; }
static void putc_(char c) { obuf[on++] = c; if (on == sizeof obuf || c == '\n') flush(); }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/* %s %c %d %u %x, with width, '-' (left) and '0'; %S: a string of at most the width (no padding) */
static void pf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { putc_(*f); continue; }
        f++;
        int left = 0, zero = 0, w = 0;
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') w = w * 10 + (*f++ - '0');
        char tmp[16];
        const char *s = tmp;
        int n = 0;
        if (*f == 's' || *f == 'S') {
            s = va_arg(ap, const char *);
            if (!s) s = "";
            while (s[n] && (*f == 's' || !w || n < w)) n++;
            if (*f == 'S') w = 0;
        } else if (*f == 'c') {
            tmp[0] = (char)va_arg(ap, int); n = 1;
        } else {
            ULONG v = va_arg(ap, ULONG);
            int neg = 0;
            if (*f == 'd' && (LONG)v < 0) { neg = 1; v = (ULONG)(-(LONG)v); }
            ULONG base = *f == 'x' ? 16 : 10;
            char r[12]; int k = 0;
            do { r[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            if (neg) r[k++] = '-';
            while (k) tmp[n++] = r[--k];
            if (zero) { while (n < w) { for (int i = n; i > 0; i--) tmp[i] = tmp[i - 1]; tmp[0] = '0'; n++; } }
        }
        if (!left) for (int i = n; i < w; i++) putc_(' ');
        for (int i = 0; i < n; i++) putc_(s[i]);
        if (left) for (int i = n; i < w; i++) putc_(' ');
    }
    va_end(ap);
}

static void kb(ULONG bytes)
{
    if (bytes >= 10u << 20) pf("%u MB", bytes >> 20);
    else pf("%u KB", bytes >> 10);
}

/* ---- the system ------------------------------------------------------------------ */
#define EXEC_VER(b)     (*(UWORD *)((UBYTE *)(b) + 20))
#define EXEC_REV(b)     (*(UWORD *)((UBYTE *)(b) + 22))
#define ATTNFLAGS(b)    (*(UWORD *)((UBYTE *)(b) + 296))
#define MEMLIST(b)      ((struct List *)((UBYTE *)(b) + 322))
#define RESLIST(b)      ((struct List *)((UBYTE *)(b) + 336))
#define DEVLIST(b)      ((struct List *)((UBYTE *)(b) + 350))
#define LIBLIST(b)      ((struct List *)((UBYTE *)(b) + 378))
#define VBLANKFREQ(b)   (*((UBYTE *)(b) + 530))
#define POWERFREQ(b)    (*((UBYTE *)(b) + 531))
#define ECLOCK(b)       (*(ULONG *)((UBYTE *)(b) + 568))
#define GFX_DISPLAYFLAGS(g) (*(UWORD *)((UBYTE *)(g) + 206))
#define GFX_CHIPREV(g)  (*((UBYTE *)(g) + 236))

static ULONG AvailMem(ULONG req) { return lvo(SYSBASE, -216, 0, req, 0, 0); }
static APTR AllocMem_(ULONG n) { return (APTR)lvo(SYSBASE, -198, n, 0x10001, 0, 0); }

static const char *kickstart(UWORD v)
{
    switch (v) {
    case 33: return "1.2";  case 34: return "1.3";  case 36: return "2.0";  case 37: return "2.04/2.05";
    case 39: return "3.0";  case 40: return "3.1";  case 45: return "3.9";  case 46: return "3.1.4";
    case 47: return "3.2";  default: return v > 47 ? "3.2 or later" : "?";
    }
}

static int emu68;                                       /* an Emu68 board was found */

static void show_sys(void)
{
    APTR sb = SYSBASE;
    UWORD af = ATTNFLAGS(sb);
    const char *cpu = (af & 0x80) ? "68060" : (af & 0x08) ? "68040" : (af & 0x04) ? "68030" :
                      (af & 0x02) ? "68020" : (af & 0x01) ? "68010" : "68000";
    const char *fpu = (af & 0x40) ? "the CPU's own" : (af & 0x20) ? "68882" : (af & 0x10) ? "68881" : 0;
    puts_("SYSTEM\n");
    pf("  Processor   %s, FPU: %s%s\n", cpu, fpu ? fpu : "none",
       emu68 ? "  (Emu68: 68k code translated to ARM on the Raspberry Pi)" : "");
    pf("  Kickstart   exec %u.%u (Kickstart %s)\n", (ULONG)EXEC_VER(sb), (ULONG)EXEC_REV(sb), kickstart(EXEC_VER(sb)));
    APTR gfx = OpenLibrary("graphics.library", 33);
    if (gfx) {
        UBYTE c = GFX_CHIPREV(gfx);
        UWORD df = GFX_DISPLAYFLAGS(gfx);
        const char *set = (c & 0x0c) == 0x0c ? "AGA" : (c & 0x03) == 0x03 ? "ECS" : (c & 0x01) ? "ECS Agnus, OCS Denise" : "OCS";
        pf("  Chipset     %s  (%s%s%s%s), %s display, graphics.library %u.%u\n", set,
           (c & 0x04) ? "Alice" : (c & 0x01) ? "ECS Agnus" : "Agnus",
           (c & 0x08) ? ", Lisa" : (c & 0x02) ? ", ECS Denise" : ", Denise",
           (c & 0x10) ? ", AA Lisa (MLISA)" : "", "",
           (df & 4) ? "PAL" : (df & 1) ? "NTSC" : "unknown", (ULONG)EXEC_VER(gfx), (ULONG)EXEC_REV(gfx));
        CloseLibrary(gfx);
    }
    pf("  Clocks      E clock %u Hz, vertical blank %u Hz, power supply %u Hz\n", ECLOCK(sb), (ULONG)VBLANKFREQ(sb), (ULONG)POWERFREQ(sb));
    APTR eb = OpenLibrary("expansion.library", 33);
    if (eb) { pf("  Expansion   expansion.library %u.%u\n", (ULONG)EXEC_VER(eb), (ULONG)EXEC_REV(eb)); CloseLibrary(eb); }
    pf("  dos         dos.library %u.%u\n", (ULONG)EXEC_VER(dos), (ULONG)EXEC_REV(dos));
    puts_("\n");
}

/* ---- memory ------------------------------------------------------------------------ */
struct MemHeader { struct Node mh_Node; UWORD mh_Attributes; APTR mh_First, mh_Lower, mh_Upper; ULONG mh_Free; };
#define MAXMEM 16
static void show_mem(void)
{
    struct { char name[28]; UWORD attr; ULONG lo, hi, fr; } m[MAXMEM];
    int n = 0;
    Forbid();
    for (struct Node *x = MEMLIST(SYSBASE)->lh_Head; x->ln_Succ && n < MAXMEM; x = x->ln_Succ) {
        struct MemHeader *h = (struct MemHeader *)x;
        int k = 0;
        if (x->ln_Name) for (; x->ln_Name[k] && k < 27; k++) m[n].name[k] = x->ln_Name[k];
        m[n].name[k] = 0;
        m[n].attr = h->mh_Attributes; m[n].lo = (ULONG)h->mh_Lower; m[n].hi = (ULONG)h->mh_Upper; m[n].fr = h->mh_Free;
        n++;
    }
    Permit();
    puts_("MEMORY\n");
    for (int i = 0; i < n; i++) {
        pf("  %-26s $%08x-$%08x  ", m[i].name[0] ? m[i].name : "(no name)", m[i].lo, m[i].hi - 1);
        kb(m[i].hi - m[i].lo); puts_(", "); kb(m[i].fr); puts_(" free ");
        UWORD a = m[i].attr;
        pf("%s%s%s%s%s%s\n", (a & 2) ? " CHIP" : "", (a & 4) ? " FAST" : "", (a & 1) ? " PUBLIC" : "",
           (a & 0x100) ? " LOCAL" : "", (a & 0x200) ? " 24BITDMA" : "", (a & 0x400) ? " KICK" : "");
    }
    puts_("  Chip: "); kb(AvailMem(2 | 0x80000)); puts_(" total, "); kb(AvailMem(2)); puts_(" free, largest "); kb(AvailMem(2 | 0x20000));
    puts_("\n  Fast: "); kb(AvailMem(4 | 0x80000)); puts_(" total, "); kb(AvailMem(4)); puts_(" free, largest "); kb(AvailMem(4 | 0x20000));
    puts_("\n\n");
}

/* ---- boards ---------------------------------------------------------------------------- */
struct ExpRom { UBYTE er_Type, er_Product, er_Flags, er_Reserved03; UWORD er_Manufacturer; ULONG er_SerialNumber; UWORD er_InitDiagVec; UBYTE er_Reserved[4]; };
struct CDev { struct Node cd_Node; UBYTE cd_Flags, cd_Pad; struct ExpRom cd_Rom; APTR cd_BoardAddr; ULONG cd_BoardSize;
              UWORD cd_SlotAddr, cd_SlotSize; APTR cd_Driver; APTR cd_NextCD; ULONG cd_Unused[4]; };
ACHECK(AOFF(struct CDev, cd_BoardAddr) == 32);
ACHECK(AOFF(struct CDev, cd_Driver) == 44);

static const char *maker(UWORD m)
{
    switch (m) {
    case 0x6d73: return "Emu68";
    case 513: case 514: case 515: return "Commodore";
    case 2017: return "Great Valley Products";
    case 2167: return "Village Tronic";
    case 4626: return "Individual Computers";
    case 8512: return "Phase 5";
    default: return 0;
    }
}
static const char *product(UWORD m, UBYTE p)
{
    if (m != 0x6d73) return 0;
    switch (p) {
    case 0x02: return "Emu68 ROM board (Emu68's drivers, alice68hdf.device)";
    case 0x10: return "Emu68 Zorro II RAM";
    case 0x11: return "Zorro III RAM (EMU68AGA)";
    case 0x68: return "network bridge (EMU68AGA)";
    case 0x69: return "USB mass storage (EMU68AGA, TinyUSB)";
    case PT_PRODUCT: return "Alice68 USB card";
    case AN_PRODUCT: return "Alice68 network card";
    default: return 0;
    }
}

#define MAXCD 24
static struct CDev cds[MAXCD];
static APTR cdaddr[MAXCD];
static char cddrv[MAXCD][32];
static int ncd;

static void collect_boards(void)
{
    APTR eb = OpenLibrary("expansion.library", 33);
    if (!eb) return;
    APTR cd = 0;
    while (ncd < MAXCD && (cd = FindConfigDev(eb, cd, -1, -1))) {
        memcpy(&cds[ncd], cd, sizeof cds[0]);
        cdaddr[ncd] = cd;
        cddrv[ncd][0] = 0;
        Forbid();
        struct Node *d = cds[ncd].cd_Driver;
        if (d && !((ULONG)d & 1) && d->ln_Name) {
            int k = 0;
            for (; d->ln_Name[k] && k < 31; k++) cddrv[ncd][k] = d->ln_Name[k];
            cddrv[ncd][k] = 0;
        }
        Permit();
        if (cds[ncd].cd_Rom.er_Manufacturer == 0x6d73) emu68 = 1;
        ncd++;
    }
    CloseLibrary(eb);
}

static void show_boards(void)
{
    pf("EXPANSION BOARDS (%u)\n", (ULONG)ncd);
    if (!ncd) puts_("  none\n");
    for (int i = 0; i < ncd; i++) {
        struct CDev *c = &cds[i];
        struct ExpRom *r = &c->cd_Rom;
        int z3 = (r->er_Type & 0xc0) == 0x80;
        const char *mk = maker(r->er_Manufacturer), *pr = product(r->er_Manufacturer, r->er_Product);
        pf("  %u. %s  $%08x  ", (ULONG)(i + 1), z3 ? "Zorro III" : "Zorro II ", (ULONG)c->cd_BoardAddr);
        kb(c->cd_BoardSize);
        pf("  manufacturer %u ($%04x)%s%s%s, product %u ($%02x)\n", (ULONG)r->er_Manufacturer, (ULONG)r->er_Manufacturer,
           mk ? " " : "", mk ? mk : "", "", (ULONG)r->er_Product, (ULONG)r->er_Product);
        if (pr) pf("     %s\n", pr);
        UBYTE sn[4] = { (UBYTE)(r->er_SerialNumber >> 24), (UBYTE)(r->er_SerialNumber >> 16), (UBYTE)(r->er_SerialNumber >> 8), (UBYTE)r->er_SerialNumber };
        int text = 1;
        for (int k = 0; k < 4; k++) if (sn[k] < 0x20 || sn[k] > 0x7e) text = 0;
        pf("     serial $%08x", r->er_SerialNumber);
        if (text) pf(" \"%c%c%c%c\"", sn[0], sn[1], sn[2], sn[3]);
        pf(", %s%s%s", (r->er_Type & 0x20) ? "memory (added to the free memory)" : "I/O, not memory",
           (r->er_Type & 0x10) ? ", boot ROM" : "", (r->er_Type & 0x08) ? ", more boards on it" : "");
        if (r->er_Type & 0x10) pf(" (diag vector $%04x)", (ULONG)r->er_InitDiagVec);
        puts_("\n     ");
        pf("state:%s%s%s%s", (c->cd_Flags & 1) ? " shut up" : " configured", (c->cd_Flags & 2) ? ", waiting for its driver" : "",
           (c->cd_Flags & 4) ? ", BAD MEMORY" : "", (c->cd_Flags & 8) ? ", processed" : "");
        if (r->er_Flags & 0x40) puts_(", cannot be shut up");
        if (z3) pf(", slot $%04x", (ULONG)c->cd_SlotAddr);
        pf(", ConfigDev at $%08x", (ULONG)cdaddr[i]);
        if (cddrv[i][0]) pf(", driver: %s", cddrv[i]);
        puts_("\n");
    }
    puts_("\n");
}

/* ---- Alice68's cards ------------------------------------------------------------------------- */
static void mac(const unsigned char *m)
{
    pf("%02x:%02x:%02x:%02x:%02x:%02x", (ULONG)m[0], (ULONG)m[1], (ULONG)m[2], (ULONG)m[3], (ULONG)m[4], (ULONG)m[5]);
}
static void ver16(ULONG v) { if (v) pf("%u.%u", v >> 16, v & 0xffff); else puts_("not opened"); }

static void show_usb(volatile UBYTE *b)
{
    volatile struct pt_hdr *h = (volatile struct pt_hdr *)b;
    pf("  USB card at $%08x\n", (ULONG)b);
    if (h->magic != PT_MAGIC) { puts_("     no data yet: the kernel has not set it up (frf_usb=neptune, or USB not started)\n"); return; }
    pf("     layout %u, %u ports, %u request slots; alice68-usb.device: ", h->version, h->nports, h->nreqs);
    ver16(h->amiga_ver);
    puts_("\n");
    int any = 0;
    for (ULONG p = 0; p < PT_PORTS && p < h->nports; p++) {
        volatile struct pt_port *pp = &h->port[p];
        if (!pp->present) continue;
        static const char *const sp[] = { "?", "full speed (12 Mbit/s)", "low speed (1.5 Mbit/s)", "high speed (480 Mbit/s)", "SuperSpeed (5 Gbit/s)" };
        pf("     port %u: %04x:%04x, %s\n", p + 1, pp->vidpid >> 16, pp->vidpid & 0xffff, sp[pp->speed < 5 ? pp->speed : 0]);
        any = 1;
    }
    if (!any) puts_("     no devices passed through (keyboards and mice stay with the kernel)\n");
    ULONG st[4] = { 0, 0, 0, 0 };
    volatile struct pt_req *rq = (volatile struct pt_req *)(b + PT_OFF_REQS);
    for (ULONG i = 0; i < PT_REQS; i++) if (rq[i].state < 4) st[rq[i].state]++;
    pf("     requests: %u free, %u waiting for the kernel, %u in progress, %u finished\n", st[0], st[1], st[2], st[3]);
    pf("     logs: kernel %u bytes, driver %u bytes written since power-on (Alice68-USBLog shows them)\n", h->klog_wr, h->alog_wr);
}

static const char *nstate(ULONG s)
{
    static const char *const n[] = { "off", "starting", "idle", "scanning", "joining", "connected", "retrying", "failed", "waiting for firmware" };
    return s < 9 ? n[s] : "?";
}

static void modes(ULONG m)
{
    static const char *const n[] = { "10 half", "10 full", "100 half", "100 full", "1000 half", "1000 full", "pause", "asym pause" };
    int first = 1;
    for (int i = 0; i < 8; i++) if (m & (1u << i)) { pf("%s%s", first ? "" : ", ", n[i]); first = 0; }
    if (first) puts_("none");
}

static void show_net(volatile UBYTE *b)
{
    volatile struct an_hdr *h = (volatile struct an_hdr *)b;
    pf("  Network card at $%08x\n", (ULONG)b);
    if (h->magic != AN_MAGIC) { puts_("     no data yet: the kernel has not set it up\n"); return; }
    static const char *const fw[] = { "none offered", "offered, not yet taken", "taken by the kernel", "refused (WiFi off?)" };
    pf("     layout %u, kernel side %u, %u units; alice68net.device: ", h->version, h->kernel_ver, h->nunits);
    ver16(h->amiga_ver);
    pf("\n     WiFi firmware: %s (%u files)%s\n", h->fw_state < 4 ? fw[h->fw_state] : "?", h->fw_count, h->trace ? "; net.trace=1" : "");
    for (ULONG u = 0; u < AN_UNITS && u < h->nunits; u++) {
        volatile struct an_unit *x = (volatile struct an_unit *)(b + AN_OFF_UNIT(u));
        if (x->kind == AN_KIND_NONE) { pf("     unit %u: not present\n", u); continue; }
        int wifi = x->kind == AN_KIND_WIFI;
        pf("     unit %u %s: %s", u, wifi ? "WiFi" : "Ethernet", nstate(x->state));
        if (x->link) pf(", link up, %u Mbit/s", x->speed_mbps);
        else puts_(", no link");
        puts_("\n        MAC "); mac((const unsigned char *)x->mac);
        if (x->factory_mac[0] | x->factory_mac[1] | x->factory_mac[2] | x->factory_mac[3] | x->factory_mac[4] | x->factory_mac[5]) {
            puts_(" (hardware's own "); mac((const unsigned char *)x->factory_mac); puts_(")");
        }
        puts_("\n");
        if (wifi) {
            if (x->ssid[0]) {
                char ss[36]; for (int k = 0; k < 35; k++) ss[k] = x->ssid[k]; ss[35] = 0;
                pf("        network \"%s\", access point ", ss); mac((const unsigned char *)x->bssid);
                ULONG s = x->security;
                pf(", channel %u, signal %d dBm, %s\n", x->channel, (ULONG)x->rssi,
                   (s & 0x10) ? "WPA3" : (s & 0x08) ? "WPA2" : (s & 0x04) ? "WPA" : (s & 0x01) ? "open" : "?");
            }
            if (x->chip) {
                char fv[96]; for (int k = 0; k < 95; k++) fv[k] = x->fwver[k]; fv[95] = 0;
                pf("        chip %x rev %u, SDIO %u MHz%s%s\n", x->chip, x->chiprev, x->sdmhz, fv[0] ? ", firmware " : "", fv);
            }
            pf("        %u networks in the last scan\n", x->scan_count);
        } else if (x->link) {
            ULONG f = x->link_flags;
            ULONG fc = f & (AN_LF_RXPAUSE | AN_LF_TXPAUSE);
            pf("        %s duplex, %s, flow control %s\n", (f & AN_LF_FULL) ? "full" : "half",
               (f & AN_LF_AUTONEG) ? "autonegotiated" : "fixed speed",
               fc == (AN_LF_RXPAUSE | AN_LF_TXPAUSE) ? "both ways" : fc == AN_LF_RXPAUSE ? "on what is received" : fc ? "on what is sent" : "off");
            puts_("        offered here: "); modes(x->link_adv); puts_("\n        offered by the other side: "); modes(x->link_lpa);
            if (x->link_downs == 1) puts_("\n        link went down once");
            else pf("\n        link went down %u times", x->link_downs);
            pf("; up since %u s after power-on\n", x->link_up_s);
        }
        pf("        in:  %u frames, ", x->rx_pkts); kb(x->rx_bytes); pf(", %u dropped\n", x->rx_drop);
        pf("        out: %u frames, ", x->tx_pkts); kb(x->tx_bytes); pf(", %u errors\n", x->tx_err);
        pf("        queues: %u of %u receive slots waiting for the stack, %u of %u send slots for the kernel%s\n",
           x->rx_head - x->rx_tail, (ULONG)AN_RX_SLOTS, x->tx_head - x->tx_tail, (ULONG)AN_TX_SLOTS,
           x->rx_on ? "" : " (no stack attached)");
        char msg[96]; for (int k = 0; k < 95; k++) msg[k] = x->msg[k]; msg[95] = 0;
        if (msg[0]) pf("        last: %s\n", msg);
    }
}

static void show_hdf(void)
{
    Forbid();
    struct HdfBase *db = FindName((UBYTE *)SYSBASE + EXEC_DEVICELIST, "alice68hdf.device");
    Permit();
    if (!db || db->magic != HDF_MAGIC) { puts_("  alice68hdf.device: not running\n"); return; }
    pf("  alice68hdf.device %u.%u (on Emu68's ROM board): card %s%s, %s\n", (ULONG)db->lib.lib_Version, (ULONG)db->lib.lib_Revision,
       db->card_dev ? db->card_dev : "?", db->cmd64 ? " (64-bit)" : " (first 4 GB)", db->readonly ? "read-only" : "read-write");
    pf("     %u hardfile unit%s", db->nunits, db->nunits == 1 ? "" : "s");
    pf(", %u card read%s, %u write%s", db->reads, db->reads == 1 ? "" : "s", db->writes, db->writes == 1 ? "" : "s");
    if (db->read_errors || db->write_errors) pf(" (%u read errors, %u write errors)", db->read_errors, db->write_errors);
    puts_("\n");
    for (ULONG i = 0; i < db->nunits && i < HDF_MAX_FILES; i++) {
        struct HUnit *u = db->unit[i];
        if (!u) continue;
        pf("     unit %u: %s, %u MB in %u piece%s%s%s\n", i, u->f.name, u->f.size >> 20, u->f.next, u->f.next == 1 ? "" : "s",
           u->single ? ", single partition" : "", u->chosen ? ", boots (hdf.boot)" : "");
    }
    for (ULONG i = 0; i < db->nfvols && i < FOLDER_MAX; i++) {
        struct FVolHead *f = (struct FVolHead *)db->fvol[i];
        if (f) pf("     folder %s: volume %s: (%s:)\n", f->path, f->vol, f->dev);
    }
}

static void show_cards(void)
{
    puts_("ALICE68\n");
    int any = 0;
    for (int i = 0; i < ncd; i++) {
        struct ExpRom *r = &cds[i].cd_Rom;
        if (r->er_Manufacturer != 0x6d73) continue;
        if (r->er_Product == PT_PRODUCT) { show_usb(cds[i].cd_BoardAddr); any = 1; }
        if (r->er_Product == AN_PRODUCT) { show_net(cds[i].cd_BoardAddr); any = 1; }
    }
    if (!any) puts_("  no Alice68 cards: not an Alice68 kernel\n");
    show_hdf();
    puts_("\n");
}

/* ---- the kernel's log, from the USB card ------------------------------------------------------- */
static void show_log(void)
{
    for (int i = 0; i < ncd; i++) {
        struct ExpRom *r = &cds[i].cd_Rom;
        if (r->er_Manufacturer != 0x6d73 || r->er_Product != PT_PRODUCT) continue;
        volatile UBYTE *b = cds[i].cd_BoardAddr;
        volatile struct pt_hdr *h = (volatile struct pt_hdr *)b;
        if (h->magic != PT_MAGIC) break;
        ULONG wr = h->klog_wr, n = wr < 4096 ? wr : 4096, start = wr - n;
        puts_("KERNEL LOG (the last 4 KB)\n");
        ULONG k = start;
        if (start) { while (k < wr && b[PT_OFF_KLOG + k % PT_KLOG_BYTES] != '\n') k++; k++; }   /* from a line start */
        for (; k < wr; k++) putc_((char)b[PT_OFF_KLOG + k % PT_KLOG_BYTES]);
        puts_("\n");
        return;
    }
    puts_("KERNEL LOG\n  not available: it is kept on the USB card (kernel8.img)\n\n");
}

/* ---- libraries, devices, resources ------------------------------------------------------------- */
struct LibInfo { char name[32]; UWORD ver, rev, open; };
static void show_list(const char *title, struct List *l, struct LibInfo *li, int max, int has_ver)
{
    int n = 0;
    Forbid();
    for (struct Node *x = l->lh_Head; x->ln_Succ && n < max; x = x->ln_Succ) {
        struct Library *lib = (struct Library *)x;
        int k = 0;
        if (x->ln_Name) for (; x->ln_Name[k] && k < 31; k++) li[n].name[k] = x->ln_Name[k];
        li[n].name[k] = 0;
        li[n].ver = has_ver ? lib->lib_Version : 0; li[n].rev = has_ver ? lib->lib_Revision : 0; li[n].open = has_ver ? lib->lib_OpenCnt : 0;
        n++;
    }
    Permit();
    pf("%s (%u)\n", title, (ULONG)n);
    for (int i = 0; i < n; i++) {
        if (has_ver) pf("  %-32s %3u.%-5u open %u\n", li[i].name, (ULONG)li[i].ver, (ULONG)li[i].rev, (ULONG)li[i].open);
        else pf("  %s\n", li[i].name);
    }
    puts_("\n");
}

/* ---- main ----------------------------------------------------------------------------------------- */
static int word_is(const char *w, int n, const char *k)
{
    int i = 0;
    for (; i < n && k[i]; i++) { char c = w[i]; if (c >= 'a' && c <= 'z') c -= 32; if (c != k[i]) return 0; }
    return i == n && !k[i];
}

LONG showconfig_main(const char *args, ULONG arglen)
{
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);
    enum { SYS = 1, MEM = 2, BRD = 4, CRD = 8, LIB = 16, LOG = 32 };
    ULONG want = 0;
    for (ULONG i = 0; i < arglen;) {
        while (i < arglen && (args[i] == ' ' || args[i] == '\t' || args[i] == '\n')) i++;
        ULONG j = i;
        while (j < arglen && args[j] != ' ' && args[j] != '\t' && args[j] != '\n') j++;
        if (j == i) break;
        const char *w = args + i;
        int n = (int)(j - i);
        if (word_is(w, n, "SYS")) want |= SYS;
        else if (word_is(w, n, "MEM")) want |= MEM;
        else if (word_is(w, n, "BOARDS")) want |= BRD;
        else if (word_is(w, n, "CARDS")) want |= CRD;
        else if (word_is(w, n, "LIBS")) want |= LIB;
        else if (word_is(w, n, "LOG")) want |= LOG;
        else if (word_is(w, n, "ALL")) want |= SYS | MEM | BRD | CRD | LIB | LOG;
        else {
            puts_("Alice68-ShowConfig [SYS] [MEM] [BOARDS] [CARDS] [LIBS] [LOG] [ALL]\n"
                  "  no words: SYS MEM BOARDS CARDS. ALL: everything, with the libraries,\n"
                  "  devices and resources in memory, and the kernel's log.\n");
            flush();
            CloseLibrary(dos);
            return n == 1 && w[0] == '?' ? 0 : 10;
        }
        i = j;
    }
    if (!want) want = SYS | MEM | BRD | CRD;
    collect_boards();
    pf("Alice68-ShowConfig %s\n\n", VERSION_STR);
    if (want & SYS) show_sys();
    if (want & MEM) show_mem();
    if (want & BRD) show_boards();
    if (want & CRD) show_cards();
    if (want & LIB) {
        struct LibInfo *li = AllocMem_(sizeof(struct LibInfo) * 96);
        if (li) {
            show_list("LIBRARIES", LIBLIST(SYSBASE), li, 96, 1);
            show_list("DEVICES", DEVLIST(SYSBASE), li, 96, 1);
            show_list("RESOURCES", RESLIST(SYSBASE), li, 96, 0);
            FreeMem(li, sizeof(struct LibInfo) * 96);
        }
    }
    if (want & LOG) show_log();
    flush();
    CloseLibrary(dos);
    return 0;
}
