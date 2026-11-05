/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-NetCheck - what a SANA-II network driver (genet.device by default) says
 * about itself, as text in a Shell: no ReAction classes needed, so it runs on any
 * AmigaOS from 3.1 on.
 *
 *   Alice68-NetCheck [DEVICE <name>] [UNIT <n>] [WATCH] [TEST]
 *
 * It opens the driver (by name first, which finds the one a TCP/IP stack has
 * loaded, then from DEVS:Networks/), asks the standard SANA-II questions -
 * S2_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_GETGLOBALSTATS, S2_GETSPECIALSTATS -
 * and says what the answers mean. genet.device's special statistics include
 * irq0_count (interrupts that reached AmigaOS) and hw_rx_pkts (packets the
 * Ethernet hardware received): together they tell a missing interrupt from a
 * missing link. WATCH repeats the key counters every second until Ctrl-C.
 * TEST brings an unconfigured interface up itself (S2_CONFIGINTERFACE with the
 * factory address), sends three broadcast test frames (EtherType 0x88B5, reserved
 * for local experiments) and watches the counters for five seconds: a sent frame
 * ends with a "transmit done" interrupt, so TEST tells a missing interrupt from a
 * missing link even on a quiet network.
 *
 * SANA-II structures and command numbers: devices/sana2.h. */
#include "amiga.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

#define S2_DEVICEQUERY        (CMD_NONSTD + 0)
#define CMD_WRITE_            3
#define S2_GETSTATIONADDRESS  (CMD_NONSTD + 1)
#define S2_CONFIGINTERFACE    (CMD_NONSTD + 2)
#define S2_GETSPECIALSTATS    (CMD_NONSTD + 12)
#define S2_GETGLOBALSTATS     (CMD_NONSTD + 13)
#define SIGBREAKF_CTRL_C      (1u << 12)
#define S2_CopyToBuff         (TAG_USER + 0xB0001)
#define S2_CopyFromBuff       (TAG_USER + 0xB0002)

struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError, ios2_PacketType;
    UBYTE ios2_SrcAddr[16], ios2_DstAddr[16];
    ULONG ios2_DataLength;
    APTR  ios2_Data, ios2_StatData, ios2_BufferManagement;
};
ACHECK(AOFF(struct IOSana2Req, ios2_SrcAddr) == 40);
ACHECK(AOFF(struct IOSana2Req, ios2_StatData) == 80);
ACHECK(sizeof(struct IOSana2Req) == 88);
struct Sana2DeviceQuery { ULONG SizeAvailable, SizeSupplied, DevQueryFormat, DeviceLevel; UWORD AddrFieldSize;
                          ULONG MTU, BPS, HardwareType, RawMTU; };
ACHECK(AOFF(struct Sana2DeviceQuery, MTU) == 18);
struct Sana2DeviceStats { ULONG PacketsReceived, PacketsSent, BadData, Overruns, Unused, UnknownTypesReceived,
                          Reconfigurations, LastStartSecs, LastStartMicro; };
struct SSRec { ULONG Type, Count; const char *String; };
struct SSHdr { ULONG RecordCountMax, RecordCountSupplied; };
#define MAXREC 96

static APTR dos;
static ULONG out;
static void puts_(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void dec(ULONG v) { char b[12]; int n = 11; b[n] = 0; do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + n); }
static void hex2(UBYTE v) { char b[3] = { "0123456789abcdef"[v >> 4], "0123456789abcdef"[v & 15], 0 }; puts_(b); }
static void pad(const char *s, int w) { int n = 0; while (s[n]) n++; puts_(s); while (n++ < w) puts_(" "); }

static APTR CreateMsgPort(void)                     { return (APTR)lvo(SYSBASE, -666, 0, 0, 0, 0); }
static void DeleteMsgPort(APTR p)                   { lvo(SYSBASE, -672, 0, 0, (ULONG)p, 0); }
static APTR CreateIORequest(APTR p, ULONG n)        { return (APTR)lvo(SYSBASE, -654, n, 0, (ULONG)p, 0); }
static void DeleteIORequest(APTR io)                { lvo(SYSBASE, -660, 0, 0, (ULONG)io, 0); }
static BYTE DoIO(APTR io)                           { return (BYTE)lvo(SYSBASE, -456, 0, 0, 0, (ULONG)io); }
static void SendIO(APTR io)                         { lvo(SYSBASE, -462, 0, 0, 0, (ULONG)io); }
static APTR CheckIO(APTR io)                        { return (APTR)lvo(SYSBASE, -468, 0, 0, 0, (ULONG)io); }
static BYTE WaitIO(APTR io)                         { return (BYTE)lvo(SYSBASE, -474, 0, 0, 0, (ULONG)io); }
static void AbortIO(APTR io)                        { lvo(SYSBASE, -480, 0, 0, 0, (ULONG)io); }
extern void nc_copybuff(void);
static APTR AllocMem_(ULONG n)                      { return (APTR)lvo(SYSBASE, -198, n, 0x10001, 0, 0); }   /* MEMF_PUBLIC|MEMF_CLEAR */

static int str_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static int ask(struct IOSana2Req *io, UWORD cmd, APTR stat)
{
    io->ios2_Req.io_Command = cmd;
    io->ios2_Req.io_Flags = 0;
    io->ios2_StatData = stat;
    io->ios2_WireError = 0;
    DoIO(io);
    return io->ios2_Req.io_Error;
}

static void err(int e, ULONG wire)
{
    puts_("error "); dec((ULONG)(LONG)e);
    if (e == 8) puts_(" (not supported)");
    else if (e == 10) puts_(" (driver offline)");
    if (wire) { puts_(", wire error "); dec(wire); }
    puts_("\n");
}

/* a special statistic by name; ~0 if the driver has none of that name */
static ULONG ss_get(struct SSHdr *h, const char *name)
{
    struct SSRec *r = (struct SSRec *)(h + 1);
    for (ULONG i = 0; i < h->RecordCountSupplied && i < MAXREC; i++)
        if (r[i].String && str_eq_ci(r[i].String, name)) return r[i].Count;
    return ~0u;
}

static ULONG nextword(const char **p, char *w, ULONG max)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    ULONG n = 0;
    int q = *s == '"';
    if (q) s++;
    while (*s && *s != '\n' && (q ? *s != '"' : (*s != ' ' && *s != '\t'))) { if (n + 1 < max) w[n++] = *s; s++; }
    if (q && *s == '"') s++;
    w[n] = 0;
    *p = s;
    return n;
}

LONG netcheck_main(const char *args, ULONG arglen)
{
    (void)arglen;
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);

    char devname[96] = "genet.device";
    ULONG unit = 0;
    int watch = 0, explicit_dev = 0, test = 0;
    char w[96];
    const char *p = args;
    while (nextword(&p, w, sizeof w)) {
        if (str_eq_ci(w, "WATCH")) watch = 1;
        else if (str_eq_ci(w, "TEST")) test = 1;
        else if (str_eq_ci(w, "UNIT") && nextword(&p, w, sizeof w)) { unit = 0; for (char *c = w; *c >= '0' && *c <= '9'; c++) unit = unit * 10 + (ULONG)(*c - '0'); }
        else if (str_eq_ci(w, "DEVICE") && nextword(&p, w, sizeof w)) { memcpy(devname, w, sizeof devname); explicit_dev = 1; }
        else if (w[0] == '?') { puts_("Alice68-NetCheck [DEVICE <name>] [UNIT <n>] [WATCH] [TEST]\n"); CloseLibrary(dos); return 0; }
        else { memcpy(devname, w, sizeof devname); explicit_dev = 1; }
    }

    puts_("Alice68-NetCheck 1.4\n");
    APTR port = CreateMsgPort();
    struct IOSana2Req *io = port ? CreateIORequest(port, sizeof *io) : 0;
    struct SSHdr *ss = AllocMem_(sizeof(struct SSHdr) + MAXREC * sizeof(struct SSRec));
    /* copy functions, so that the driver can take the test frames */
    static const struct TagItem bufmgmt[] = { { S2_CopyToBuff, (ULONG)nc_copybuff }, { S2_CopyFromBuff, (ULONG)nc_copybuff },
                                              { TAG_DONE, 0 } };
    if (!io || !ss) { puts_("out of memory\n"); goto out; }
    io->ios2_BufferManagement = (APTR)bufmgmt;

    /* By name first: that is the copy a TCP/IP stack has loaded. Only if no driver of
       that name is loaded at all, from DEVS:Networks/ - opening a path while one is
       loaded makes AmigaOS load a second copy, which would drive the same hardware
       behind the stack's back. */
    Forbid();
    int loaded = FindName((UBYTE *)SYSBASE + EXEC_DEVICELIST, "genet.device") != 0;
    Permit();
    LONG e = OpenDevice(devname, unit, io, 0);
    if (e && !explicit_dev && !loaded) {
        memcpy(devname, "DEVS:Networks/genet.device", 27);
        e = OpenDevice(devname, unit, io, 0);
    }
    puts_("Device:    "); puts_(devname); puts_(" unit "); dec(unit);
    if (e) {
        puts_(" - cannot open it (error "); dec((ULONG)(LONG)e); puts_(")\n");
        if (loaded && !explicit_dev)
            puts_("           genet.device is loaded - your TCP/IP stack has it - but refused a second\n"
                  "           opener: the stack probably opened it for itself alone (exclusive).\n"
                  "           The driver is running; check the network from the stack's own status.\n");
        else
            puts_("           Is the driver in DEVS:Networks/? A SANA-II genet.device is version 3.x.\n");
        goto out;
    }
    if (!loaded && !explicit_dev)
        puts_(" (not loaded before: no TCP/IP stack had it open)");
    struct Library *lib = io->ios2_Req.io_Device;
    puts_(", version "); dec(lib->lib_Version); puts_("."); dec(lib->lib_Revision); puts_("\n");
    if (lib->lib_IdString) { puts_("           "); puts_((const char *)lib->lib_IdString); }
    {   /* the id string ends with its own line break, mostly */
        const char *id = lib->lib_IdString;
        int n = 0;
        if (id) while (id[n]) n++;
        if (!id || !n || id[n - 1] != '\n') puts_("\n");
    }
    if (lib->lib_Version >= 4 && str_eq_ci(lib->lib_Node.ln_Name ? lib->lib_Node.ln_Name : "", "genet.device"))
        puts_("           genet.device 4.x is the netdev driver: on Alice68 use the SANA-II driver, 3.x\n");

    struct Sana2DeviceQuery q;
    memset(&q, 0, sizeof q);
    q.SizeAvailable = sizeof q;
    puts_("Query:     ");
    e = ask(io, S2_DEVICEQUERY, &q);
    if (e) err(e, io->ios2_WireError);
    else {
        puts_(q.HardwareType == 1 ? "Ethernet" : "hardware type "); if (q.HardwareType != 1) dec(q.HardwareType);
        puts_(", MTU "); dec(q.MTU); puts_(", line rate ");
        if (q.BPS >= 1000000) { dec(q.BPS / 1000000); puts_(" Mbit/s"); } else if (q.BPS) { dec(q.BPS); puts_(" bit/s"); } else puts_("0 (no link?)");
        puts_("\n");
    }

    /* SANA-II: SrcAddr is the address in use - zero until a stack configures the
       interface (S2_CONFIGINTERFACE) - and DstAddr the factory one */
    int configured = 0;
    UBYTE factory[16];
    int bytes = (q.AddrFieldSize ? q.AddrFieldSize : 48) / 8;
    if (bytes > 16) bytes = 16;
    memset(factory, 0, sizeof factory);
    puts_("Address:   ");
    e = ask(io, S2_GETSTATIONADDRESS, 0);
    if (e) err(e, io->ios2_WireError);
    else {
        for (int i = 0; i < bytes; i++) { factory[i] = io->ios2_DstAddr[i]; if (io->ios2_SrcAddr[i]) configured = 1; }
        puts_("factory ");
        for (int i = 0; i < bytes; i++) { if (i) puts_(":"); hex2(factory[i]); }
        if (configured) {
            puts_(", in use ");
            for (int i = 0; i < bytes; i++) { if (i) puts_(":"); hex2(io->ios2_SrcAddr[i]); }
            puts_("\n");
        } else {
            puts_("\n           not configured: no TCP/IP stack has brought this interface up\n");
        }
    }
    if (test && !e && !configured) {
        puts_("Test:      bringing the interface up with the factory address... ");
        memcpy(io->ios2_SrcAddr, factory, 16);
        int ce = ask(io, S2_CONFIGINTERFACE, 0);
        if (ce) err(ce, io->ios2_WireError);
        else { puts_("online\n"); configured = 1; }
    } else if (test && configured) {
        puts_("Test:      already configured by a TCP/IP stack - testing it as it is\n");
    }
    if (test && configured) {
        /* three broadcast frames: each one ends with a transmit-done interrupt */
        static UBYTE frame[46] = "Alice68-NetCheck test frame - please ignore";
        puts_("Test:      sending 3 broadcast test frames... ");
        int sent = 0, lasterr = 0;
        for (int i = 0; i < 3; i++) {
            io->ios2_Req.io_Command = CMD_WRITE_;
            io->ios2_Req.io_Flags = 0;
            io->ios2_PacketType = 0x88b5;
            memset(io->ios2_DstAddr, 0xff, 6);
            io->ios2_DataLength = sizeof frame;
            io->ios2_Data = frame;
            io->ios2_WireError = 0;
            SendIO(io);
            int waited = 0;
            while (!CheckIO(io) && waited < 40) { Delay(dos, 5); waited++; }      /* up to 4 s */
            if (!CheckIO(io)) {
                AbortIO(io);
                WaitIO(io);
                puts_("no answer in 4 s (the driver holds the frame: no link?)\n");
                lasterr = -1;
                break;
            }
            int we = WaitIO(io);
            if (we) { lasterr = we; puts_("frame "); dec((ULONG)i + 1); puts_(": "); err(we, io->ios2_WireError); break; }
            sent++;
        }
        if (sent == 3) puts_("sent\n");
        else if (lasterr == 0) puts_("\n");
    }
    int rounds = test ? 6 : -1;

    for (int round = 0;; round++) {
        struct Sana2DeviceStats g;
        memset(&g, 0, sizeof g);
        int ge = ask(io, S2_GETGLOBALSTATS, &g);
        memset(ss, 0, sizeof *ss);
        ss->RecordCountMax = MAXREC;
        int se = ask(io, S2_GETSPECIALSTATS, ss);
        ULONG irq = se ? ~0u : ss_get(ss, "irq0_count"), hwrx = se ? ~0u : ss_get(ss, "hw_rx_pkts");
        ULONG hwtx = se ? ~0u : ss_get(ss, "hw_tx_pkts");

        if (round == 0) {
            puts_("Packets:   ");
            if (ge) err(ge, io->ios2_WireError);
            else {
                puts_("received "); dec(g.PacketsReceived); puts_(", sent "); dec(g.PacketsSent);
                puts_(", bad "); dec(g.BadData); puts_(", overruns "); dec(g.Overruns);
                puts_(", unknown types "); dec(g.UnknownTypesReceived); puts_("\n");
            }
            puts_("Counters:  ");
            if (se) err(se, io->ios2_WireError);
            else {
                puts_("\n");
                struct SSRec *r = (struct SSRec *)(ss + 1);
                for (ULONG i = 0; i < ss->RecordCountSupplied && i < MAXREC; i++) {
                    puts_("  "); pad(r[i].String ? r[i].String : "?", 20); dec(r[i].Count); puts_("\n");
                }
            }
            if (test && rounds > 0) { puts_("\nWatching for 5 seconds:\n"); goto next; }
          verdict:
            puts_("\nWhat it means:\n");
            if (irq == ~0u || hwrx == ~0u)
                puts_("  This driver does not report interrupt and hardware counters (genet.device 3.x does).\n");
            else if (!configured)
                puts_("  The interface is not up yet, so the driver has not switched its interrupts on and\n"
                      "  these counters say nothing about Alice68. Bring it up in your TCP/IP stack\n"
                      "  (genet.device, unit 0), or run Alice68-NetCheck TEST to bring it up here and check\n"
                      "  the hardware and interrupts.\n");
            else if (hwrx == 0 && hwtx == 0)
                puts_("  The interface is up but the Ethernet hardware sent and received nothing: no link.\n"
                      "  Check the cable and the port's LEDs (both ends). If they are lit, the Ethernet PHY\n"
                      "  may not have started: please report it with this output.\n");
            else if (irq == 0)
                puts_("  The Ethernet hardware has traffic, but NO interrupt reached AmigaOS.\n"
                      "  That is an Alice68 problem: please report it with this output.\n");
            else if (test)
                puts_("  The test frames went out and their interrupts reached AmigaOS: hardware, driver\n"
                      "  and interrupts work. If the network still fails, it is the TCP/IP stack's\n"
                      "  settings (interface, address, DHCP).\n");
            else
                puts_("  Interrupts reach AmigaOS and the hardware has traffic: the driver works.\n"
                      "  If the network still fails, look at the TCP/IP stack's settings (address, DHCP).\n");
            if (!watch) break;
            puts_("\nWatching (Ctrl-C stops):\n");
        }
      next:
        if (round > 0 || watch) {
            puts_("  irq0 "); dec(irq == ~0u ? 0 : irq); puts_("   hw rx "); dec(hwrx == ~0u ? 0 : hwrx);
            puts_("   hw tx "); dec(hwtx == ~0u ? 0 : hwtx); puts_("   received "); dec(g.PacketsReceived);
            puts_("   sent "); dec(g.PacketsSent); puts_("\n");
        }
        if (rounds > 0 && --rounds == 0) {           /* TEST: the verdict on what the five seconds showed */
            rounds = -1;
            goto verdict;
        }
        Delay(dos, 50);
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) { puts_("***Break\n"); break; }
    }
    CloseDevice(io);
out:
    if (ss) FreeMem(ss, sizeof(struct SSHdr) + MAXREC * sizeof(struct SSRec));
    if (io) DeleteIORequest(io);
    if (port) DeleteMsgPort(port);
    CloseLibrary(dos);
    return 0;
}
