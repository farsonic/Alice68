/* SPDX-License-Identifier: GPL-2.0-only
 *
 * C:Alice68 - Alice68's Shell command: one command with subcommands, as Riptide's
 * `Riptide DNS`. The settings window is SYS:Prefs/Alice68Prefs.
 *
 *   Alice68 STATUS [LOOP]                 the kernel's status page
 *   Alice68 LOG [ALL] [LINES=n] [MATCH=text] [FOLLOW]   the kernel's and the drivers' logs
 *   Alice68 JOYTEST [ONCE] [MOUSEPORT]    the joystick port as games read it
 *   Alice68 WIFI [START|SCAN|JOIN ...|RECONNECT|DISCONNECT|LOG|OFF]   the kernel's WiFi
 *   Alice68 CARD [INFO|LIST|...|SET|BOOT|KICKSTART|CONFIG|RESTART]   the SD card, cmdline.txt, config.txt
 *   Alice68 BT [LIST|SCAN|PAIR|CONNECT|FORGET|KEEP]   the kernel's Bluetooth
 *   Alice68 HDF                           the hardfiles, folders and CD images
 *   Alice68 NETCHECK ...                  checks a network driver (SANA-II / netdev)
 *   Alice68 SHOWCONFIG ...                boards, memory and Alice68's cards
 *   Alice68 MONITOR [DUMP]                tasks, backtraces and the logs (DUMP: as text)
 *   Alice68 WHEEL [NEWMOUSE]              the mouse wheel helper (run again: stop it)
 *   Alice68 HELP                          the list
 *
 * Each subcommand is the tool it was before (Alice68-Status, Alice68-USBLog, ...), linked in
 * with its symbols kept to itself (build.sh: objcopy --keep-global-symbol=<its main>).
 * Started from Workbench (the WBStartup project icon), it is the wheel helper.
 */
#include "amiga.h"

#define VERSION_STR "1.0 (17.11.2026)"
static const char vers[] __attribute__((used)) = "$VER: Alice68 " VERSION_STR;

LONG status_main(const char *a, ULONG n);
LONG usblog_main(const char *a, ULONG n);
LONG joytest_main(const char *a, ULONG n);
LONG wifitool_main(const char *a, ULONG n);
LONG hdfinfo_main(const char *a, ULONG n);
LONG netcheck_main(const char *a, ULONG n);
LONG showconfig_main(const char *a, ULONG n);
LONG monitor_main(const char *a, ULONG n);
LONG wheel_main(const char *a, ULONG n);
LONG card_main(const char *a, ULONG n);
LONG bt_main(const char *a, ULONG n);

static const struct { const char *name; LONG (*fn)(const char *, ULONG); const char *what; } cmds[] = {
    { "STATUS",     status_main,     "[LOOP]  the kernel's status page: cores, temperature, JIT, network" },
    { "LOG",        usblog_main,     "[ALL] [LINES=n] [MATCH=text] [FOLLOW]  the kernel's and drivers' logs" },
    { "JOYTEST",    joytest_main,    "[ONCE] [MOUSEPORT]  the joystick port as games read it, live" },
    { "WIFI",       wifitool_main,   "[START|SCAN|JOIN \"name\" \"pass\" [CC]|RECONNECT|DISCONNECT|LOG|OFF]" },
    { "CARD",       card_main,       "[INFO|LIST|DELETE|RENAME|MKDIR|OPTIONS|SET|BOOT|KICKSTART|CONFIG|RESTART]" },
    { "BT",         bt_main,         "[LIST|SCAN|PAIR addr|CONNECT addr|FORGET addr|KEEP]  Bluetooth" },
    { "HDF",        hdfinfo_main,    "the hardfiles, folders and CD images on the SD card" },
    { "NETCHECK",   netcheck_main,   "[device] [unit] [TEST]  checks a network driver" },
    { "SHOWCONFIG", showconfig_main, "[SYS] [MEM] [BOARDS] [CARDS] [LIBS] [LOG] [ALL]" },
    { "MONITOR",    monitor_main,    "[DUMP]  tasks, backtraces and logs (DUMP: as text)" },
    { "WHEEL",      wheel_main,      "[NEWMOUSE]  the mouse wheel helper (run again: stop it)" },
};
#define NCMDS (sizeof cmds / sizeof cmds[0])
#define PR_CLI 172

static inline APTR FindTask(const char *n) { return (APTR)lvo(SYSBASE, -294, 0, 0, 0, (ULONG)n); }

static int ieq_word(const char *a, ULONG n, const char *w)   /* a[0..n) is w, any case */
{
    ULONG i = 0;
    for (; i < n && w[i]; i++) { char c = a[i]; if (c >= 'a' && c <= 'z') c -= 32; if (c != w[i]) return 0; }
    return i == n && !w[i];
}
static LONG slen(const char *s) { LONG n = 0; while (s[n]) n++; return n; }

static void help(APTR dos, int unknown, const char *a, ULONG n)
{
    ULONG out = Output(dos);
    if (unknown) { Write(dos, out, "Alice68: no subcommand \"", 24); Write(dos, out, a, (LONG)n); Write(dos, out, "\"\n", 2); }
    static const char head[] = "Alice68 " VERSION_STR ": Alice68's Shell command (settings: SYS:Prefs/Alice68Prefs)\n";
    Write(dos, out, head, sizeof head - 1);
    for (ULONG i = 0; i < NCMDS; i++) {
        LONG k = slen(cmds[i].name);
        Write(dos, out, "  Alice68 ", 10); Write(dos, out, cmds[i].name, k);
        for (; k < 11; k++) Write(dos, out, " ", 1);
        Write(dos, out, cmds[i].what, slen(cmds[i].what)); Write(dos, out, "\n", 1);
    }
    static const char tail[] = "  Alice68 HELP       this list\n";
    Write(dos, out, tail, sizeof tail - 1);
}

LONG alice68_main(const char *args, ULONG arglen)
{
    UBYTE *me = FindTask(0);
    if (!*(ULONG *)(me + PR_CLI)) return wheel_main(args, 0);     /* from Workbench: the wheel helper */
    ULONG i = 0;
    while (i < arglen && (args[i] == ' ' || args[i] == '\t')) i++;
    ULONG s = i;
    while (i < arglen && args[i] != ' ' && args[i] != '\t' && args[i] != '\n') i++;
    ULONG wl = i - s;
    while (i < arglen && (args[i] == ' ' || args[i] == '\t')) i++;
    for (ULONG c = 0; c < NCMDS; c++)
        if (wl && ieq_word(args + s, wl, cmds[c].name)) return cmds[c].fn(args + i, arglen - i);
    APTR dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    int unknown = wl && !ieq_word(args + s, wl, "HELP") && !ieq_word(args + s, wl, "?");
    help(dos, unknown, args + s, wl);
    CloseLibrary(dos);
    return unknown ? 10 : 0;
}
