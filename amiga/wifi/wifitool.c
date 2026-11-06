/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-WiFi: the Shell side of alice68wifi.device.
 *
 *   Alice68-WiFi                      status (never starts the chip)
 *   Alice68-WiFi START                start the chip (STEP=n in the prefs stops after step n)
 *   Alice68-WiFi SCAN                 networks in range
 *   Alice68-WiFi JOIN "name" "pass"   save ENV(ARC):alice68wifi.prefs and connect
 *   Alice68-WiFi RECONNECT            read the prefs again and connect
 *   Alice68-WiFi DISCONNECT
 *   Alice68-WiFi LOG                  the driver's log
 *   Alice68-WiFi OFF                  switch the chip off */
#include "wifios.h"
#include "wifi_api.h"
#include "fmac.h"

#define DEVNAME "alice68wifi.device"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

static APTR dos;
static ULONG out;
static void put(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void dec(LONG v)
{
    char b[13]; int n = 12, neg = v < 0; ULONG u = neg ? (ULONG)-v : (ULONG)v;
    b[n] = 0;
    do { b[--n] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--n] = '-';
    put(b + n);
}
static void hex2(UBYTE v) { char b[3] = { "0123456789abcdef"[v >> 4], "0123456789abcdef"[v & 15], 0 }; put(b); }
static void mac(const UBYTE *m) { for (int i = 0; i < 6; i++) { if (i) put(":"); hex2(m[i]); } }
static void pad(const char *s, int w) { int n = 0; while (s[n]) n++; put(s); while (n++ < w) put(" "); }
static int sieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* next word of the argument line; "quoted" words keep their spaces */
static int word(const char **p, char *w, int max)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '\n') { *p = s; return 0; }
    int n = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && *s != '\n') { if (n < max - 1) w[n++] = *s; s++; }
        if (*s == '"') s++;
    } else
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') { if (n < max - 1) w[n++] = *s; s++; }
    w[n] = 0;
    *p = s;
    return 1;
}

static const char *state_name(UWORD s)
{
    static const char *const n[] = { "off", "starting", "idle", "scanning", "joining", "connected", "waiting to retry", "failed" };
    return s < 8 ? n[s] : "?";
}

static void security(UWORD s)
{
    if (s & SEC_WPA3) put(s & SEC_WPA2 ? "WPA2/WPA3" : "WPA3");
    else if (s & SEC_WPA2) put("WPA2");
    else if (s & SEC_WPA) put("WPA");
    else if (s & SEC_8021X) put("Enterprise");
    else if (s & SEC_WEP) put("WEP");
    else put("open");
}

static int status(struct IOStdReq *io, struct WifiStatus *s)
{
    io->io.io_Command = WIFI_CMD_STATUS;
    io->io_Data = s;
    io->io_Length = sizeof *s;
    return DoIO(io);                    /* answered at once by the device, never queued */
}

/* A request the driver's process answers: waited for at most secs seconds.
   If the process is stuck the request cannot be taken back, so the tool
   reports it and leaves without freeing it (stuck = 1). */
static int stuck;
static LONG ask(struct IOStdReq *io, int secs)
{
    SendIO(io);
    for (int i = 0; i < secs * 10; i++) {
        if (CheckIO(io)) { WaitIO(io); return io->io.io_Error; }
        Delay(dos, 5);
    }
    AbortIO(io);                        /* still queued: taken back */
    for (int i = 0; i < 10; i++) {
        if (CheckIO(io)) { WaitIO(io); put("the driver did not answer\n"); return -1; }
        Delay(dos, 5);
    }
    put("the driver is stuck: it has not answered for "); dec(secs);
    put(" s. Alice68-WiFi LOG in another Shell shows its last lines.\n");
    stuck = 1;
    return -1;
}

static void show(struct WifiStatus *s)
{
    put("State:     "); put(state_name(s->ws_State)); put("\n");
    if (s->ws_Message[0]) { put("           "); put(s->ws_Message); put("\n"); }
    if (s->ws_Chip) {
        put("Chip:      "); hex2((UBYTE)(s->ws_Chip >> 8)); hex2((UBYTE)s->ws_Chip); put(" rev "); dec(s->ws_ChipRev);
        put(", SDIO "); dec(s->ws_SdMhz); put(" MHz, "); put(s->ws_Interrupts ? "interrupts" : "polling"); put("\n");
    }
    if (s->ws_FwVersion[0]) { put("Firmware:  "); put(s->ws_FwVersion); put("\n"); }
    if (s->ws_Mac[0] | s->ws_Mac[1] | s->ws_Mac[2]) { put("MAC:       "); mac(s->ws_Mac); put("\n"); }
    if (s->ws_Ssid[0]) { put("Network:   \""); put(s->ws_Ssid); put("\""); }
    if (s->ws_State == WS_CONNECTED) {
        put(", "); security(s->ws_Security); put(", channel "); dec(s->ws_Channel);
        put(", "); dec(s->ws_Rssi); put(" dBm, "); dec(s->ws_RateMbps); put(" Mbit/s\n");
        put("Access pt: "); mac(s->ws_Bssid); put("\n");
    } else if (s->ws_Ssid[0]) put("\n");
    put("TCP/IP:    "); put(s->ws_Attached ? "attached (netdev)" : "no stack attached"); put("\n");
    put("Packets:   received "); dec((LONG)s->ws_RxPackets); put(", sent "); dec((LONG)s->ws_TxPackets);
    put(", dropped "); dec((LONG)s->ws_RxDropped); put(", errors "); dec((LONG)s->ws_TxErrors); put(", SDIO errors "); dec((LONG)s->ws_SdErrors); put("\n");
}

static int msame(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

/* state and message, each change printed; Ctrl-C stops waiting */
static int wait_state(struct IOStdReq *io, struct WifiStatus *s, int want_connected, int secs)
{
    UWORD last = 0xffff;
    static char lastmsg[96];
    lastmsg[0] = 0;
    for (int i = 0; i < secs * 4; i++) {
        if (status(io, s)) return -1;
        if (s->ws_State != last || !msame(s->ws_Message, lastmsg)) {
            last = s->ws_State;
            for (int k = 0; k < 96; k++) lastmsg[k] = s->ws_Message[k];
            lastmsg[95] = 0;
            put("  "); put(state_name(last));
            if (s->ws_Message[0]) { put(": "); put(s->ws_Message); }
            put("\n");
        }
        if (s->ws_State == WS_FAILED) return -1;
        if (!want_connected && s->ws_State != WS_STARTING && s->ws_State != WS_OFF) return 0;
        if (want_connected && s->ws_State == WS_CONNECTED) return 0;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) return -2;
        Delay(dos, 12);
    }
    return 1;
}

/* the prefs file without its SSID/PASSWORD lines, then the new ones */
static int save_prefs(const char *ssid, const char *pass)
{
    static char buf[2048];
    LONG n = 0;
    ULONG fh = DosOpen(dos, "ENVARC:alice68wifi.prefs", MODE_OLDFILE);
    if (!fh) fh = DosOpen(dos, "ENV:alice68wifi.prefs", MODE_OLDFILE);
    if (fh) { n = DosRead(dos, fh, buf, sizeof buf - 300); DosClose(dos, fh); if (n < 0) n = 0; }
    static char outb[2400];
    LONG o = 0;
    for (LONG i = 0; i < n; ) {
        LONG s = i;
        while (i < n && buf[i] != '\n') i++;
        LONG e = i < n ? i + 1 : i;
        const char *l = buf + s;
        while (*l == ' ' || *l == '\t') l++;
        char key[16]; int k = 0;
        while (k < 15 && l + k < buf + i && l[k] != '=' && l[k] != ' ') { key[k] = l[k]; k++; }
        key[k] = 0;
        if (!sieq(key, "SSID") && !sieq(key, "PASSWORD") && !sieq(key, "PSK") && !sieq(key, "PASSPHRASE")) {
            for (LONG j = s; j < e; j++) outb[o++] = buf[j];
            if (e == i && i > s) outb[o++] = '\n';
        }
        i = e;
    }
    const char *parts[] = { "SSID=\"", ssid, "\"\nPASSWORD=\"", pass, "\"\n", 0 };
    for (int p = 0; parts[p]; p++) for (const char *c = parts[p]; *c && o < (LONG)sizeof outb - 1; ) outb[o++] = *c++;
    int ok = 0;
    static const char *const files[] = { "ENV:alice68wifi.prefs", "ENVARC:alice68wifi.prefs" };
    for (int f = 0; f < 2; f++) {
        fh = DosOpen(dos, files[f], 1006);     /* MODE_NEWFILE */
        if (!fh) continue;
        if (Write(dos, fh, outb, o) == o) ok++;
        DosClose(dos, fh);
    }
    return ok;
}

LONG wifitool_main(const char *args, ULONG arglen)
{
    (void)arglen;
    dos = OpenLibrary("dos.library", 37);
    if (!dos) return 20;
    out = Output(dos);
    char cmd[32] = "", a1[64] = "", a2[80] = "";
    const char *p = args;
    word(&p, cmd, sizeof cmd);
    word(&p, a1, sizeof a1);
    word(&p, a2, sizeof a2);
    if (cmd[0] == '?' || (cmd[0] && !sieq(cmd, "STATUS") && !sieq(cmd, "START") && !sieq(cmd, "SCAN") && !sieq(cmd, "JOIN") &&
        !sieq(cmd, "RECONNECT") && !sieq(cmd, "DISCONNECT") && !sieq(cmd, "LOG") && !sieq(cmd, "OFF"))) {
        put("Alice68-WiFi [STATUS | START | SCAN | JOIN \"network\" \"password\" | RECONNECT | DISCONNECT | LOG | OFF]\n");
        CloseLibrary(dos);
        return cmd[0] == '?' ? 0 : 10;
    }
    if (sieq(cmd, "JOIN") && !a1[0]) { put("JOIN needs the network name (and its password, if it has one)\n"); CloseLibrary(dos); return 10; }

    LONG rc = 0;
    APTR port = CreateMsgPort();
    struct IOStdReq *io = port ? CreateIORequest(port, sizeof *io) : 0;
    put("opening " DEVNAME "...");     /* if the machine stops here, it is the open */
    LONG oerr = io ? OpenDevice(DEVNAME, 0, io, 0) : 1;
    put("\r\x1b[K");
    if (oerr) {
        put("cannot open " DEVNAME ": is it in DEVS:?\n");
        if (io) DeleteIORequest(io);
        if (port) DeleteMsgPort(port);
        CloseLibrary(dos);
        return 20;
    }
    static struct WifiStatus st;
    if (sieq(cmd, "LOG")) {
        static char log[4096];
        io->io.io_Command = WIFI_CMD_LOG;
        io->io_Data = log;
        io->io_Length = sizeof log;
        if (!DoIO(io)) Write(dos, out, log, (LONG)io->io_Actual);
    } else if (sieq(cmd, "OFF")) {
        io->io.io_Command = WIFI_CMD_OFF;
        if (!ask(io, 10)) put("WiFi switched off\n");
        else if (!stuck) put("not switched off (Alice68-WiFi says why)\n");
    } else if (sieq(cmd, "START")) {
        if (status(io, &st)) { put("no status\n"); rc = 10; }
        else if (st.ws_State != WS_OFF) { put("already started (Alice68-WiFi OFF and START to start again)\n"); show(&st); }
        else {
            io->io.io_Command = WIFI_CMD_START;
            if (ask(io, 10)) rc = 10;
            else {
                put("Starting the WiFi chip:\n");
                int r = wait_state(io, &st, 0, 40);
                if (r) { put(r == -2 ? "stopped watching (it goes on)\n" : r > 0 ? "still starting - Alice68-WiFi shows how it goes\n" : "did not start\n"); rc = 5; }
                show(&st);
            }
        }
    } else {
        if (status(io, &st)) st.ws_State = WS_OFF;
        if (st.ws_State == WS_STARTING) { put("The WiFi chip is starting:\n"); wait_state(io, &st, 0, 40); }
        if (sieq(cmd, "SCAN") && st.ws_State == WS_OFF) { put("the WiFi chip is off: Alice68-WiFi START first\n"); rc = 10; }
        else if (sieq(cmd, "SCAN")) {
            static struct WifiScanEntry e[40];
            io->io.io_Command = WIFI_CMD_SCAN;
            io->io_Data = e;
            io->io_Length = sizeof e;
            if (ask(io, 20)) { if (!stuck) put("scan failed (is the chip running? Alice68-WiFi LOG says why)\n"); rc = 10; }
            else {
                put("Signal  Ch  Security   Access point       Network\n");
                for (ULONG i = 0; i < io->io_Actual; i++) {
                    char b[8]; int n = 0; LONG v = e[i].we_Rssi;
                    if (v < 0) { b[n++] = '-'; v = -v; }
                    if (v >= 100) b[n++] = (char)('0' + v / 100);
                    if (v >= 10) b[n++] = (char)('0' + v / 10 % 10);
                    b[n++] = (char)('0' + v % 10); b[n] = 0;
                    pad(b, 4); put("dBm ");
                    char c[4]; n = 0; v = e[i].we_Channel;
                    if (v >= 100) c[n++] = (char)('0' + v / 100);
                    if (v >= 10) c[n++] = (char)('0' + v / 10 % 10);
                    c[n++] = (char)('0' + v % 10); c[n] = 0;
                    pad(c, 4);
                    UWORD s = e[i].we_Security;
                    pad(s & SEC_WPA3 ? (s & SEC_WPA2 ? "WPA2/WPA3" : "WPA3") : s & SEC_WPA2 ? "WPA2" : s & SEC_WPA ? "WPA" :
                        s & SEC_8021X ? "Enterprise" : s & SEC_WEP ? "WEP" : "open", 11);
                    mac(e[i].we_Bssid); put("  "); put(e[i].we_Ssid); put("\n");
                }
                if (!io->io_Actual) put("no networks found\n");
            }
        } else if (sieq(cmd, "JOIN") || sieq(cmd, "RECONNECT")) {
            if (sieq(cmd, "JOIN")) {
                if (!save_prefs(a1, a2)) put("could not write ENV:alice68wifi.prefs\n");
                else put("Saved in ENV: and ENVARC:alice68wifi.prefs\n");
            }
            io->io.io_Command = WIFI_CMD_RECONNECT;
            if (ask(io, 20)) { rc = 10; goto done; }
            if (!status(io, &st) && (st.ws_State == WS_OFF || st.ws_State == WS_STARTING)) put("Starting the WiFi chip:\n");
            int r = wait_state(io, &st, 1, 60);
            if (r) { put(r > 0 ? "not connected yet - Alice68-WiFi shows how it goes\n" : "not connected\n"); rc = 5; }
            show(&st);
        } else if (sieq(cmd, "DISCONNECT")) {
            io->io.io_Command = WIFI_CMD_DISCONNECT;
            if (!ask(io, 10)) put("disconnected\n");
        } else {
            if (status(io, &st)) { put("no status\n"); rc = 10; }
            else show(&st);
        }
    }
done:
    if (stuck) { CloseLibrary(dos); return 20; }   /* the request is still the driver's: nothing freed */
    CloseDevice(io);
    DeleteIORequest(io);
    DeleteMsgPort(port);
    CloseLibrary(dos);
    return rc;
}
