/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68-JoyTest - what a game reads from the two joystick ports, live, in a Shell: the
 * directions from JOY1DAT / JOY0DAT, the fire buttons from CIA-A, and the second and third
 * buttons from POTGOR. The same registers games read, so a Bluetooth or USB pad that moves
 * here moves in games too.
 *
 *   Alice68-JoyTest            the joystick port: a line each time it changes, until Ctrl-C
 *   Alice68-JoyTest MOUSEPORT  the mouse port too, read as a joystick (a second pad there);
 *                              a mouse moving there looks like random directions
 *   Alice68-JoyTest ONCE       as they are now
 *
 * The joystick port's pins 5 and 9 (POTGO's RX/RY bits) are taken from potgo.resource and
 * made inputs while it runs; the mouse port's are left to the system (its right button).
 */
#include "amiga.h"

#define VERSION_STR "1.1 (16.11.2026)"
static const char vers[] __attribute__((used)) = "$VER: Alice68-JoyTest " VERSION_STR;

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

#define JOY0DAT ((volatile UWORD *)0xDFF00A)
#define JOY1DAT ((volatile UWORD *)0xDFF00C)
#define POTGOR  ((volatile UWORD *)0xDFF016)
#define CIAAPRA ((volatile UBYTE *)0xBFE001)

static APTR dos; static ULONG out;
static void puts_(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void hex4(ULONG v) { char b[5]; for (int i = 0; i < 4; i++) b[i] = "0123456789ABCDEF"[(v >> ((3 - i) * 4)) & 15]; b[4] = 0; puts_(b); }
static int brk(void) { return (SetSignal(0, 1u << 12) & (1u << 12)) != 0; }
static inline APTR OpenResource(const char *n) { return (APTR)lvo(SYSBASE, -498, 0, 0, 0, (ULONG)n); }
/* potgo.resource */
static APTR pot;
static ULONG AllocPotBits(ULONG bits)       { return lvo(pot, -6, bits, 0, 0, 0); }
static void FreePotBits(ULONG bits)         { lvo(pot, -12, bits, 0, 0, 0); }
static void WritePotgo(ULONG w, ULONG mask) { lvo(pot, -18, w, mask, 0, 0); }

/* one port: bits 0-3 up, down, left, right; 4-6 fire 1, 2, 3 */
static ULONG port_state(UWORD joy, int fire, int b2, int b3)
{
    int right = (joy >> 1) & 1, left = (joy >> 9) & 1;
    int down = right ^ (joy & 1), up = left ^ ((joy >> 8) & 1);
    return (ULONG)(up | down << 1 | left << 2 | right << 3 | fire << 4 | b2 << 5 | b3 << 6);
}
static void show_port(const char *name, ULONG s, int has3)
{
    static const char *const w[] = { "UP", "DOWN", "LEFT", "RIGHT", "FIRE", "FIRE2", "FIRE3" };
    puts_(name);
    for (int i = 0; i < 7; i++) {
        if (i == 6 && !has3) break;
        const char *t = (s >> i) & 1 ? w[i] : "-";
        int n = 0; while (t[n]) n++;
        puts_(t); while (n++ < 6) puts_(" ");
    }
}

static int word(const char *a, ULONG n, const char *w)
{
    ULONG k = 0; while (w[k]) k++;
    for (ULONG i = 0; i + k <= n; i++) {
        ULONG j = 0;
        while (j < k && (a[i + j] | 32) == w[j]) j++;
        if (j == k) return 1;
    }
    return 0;
}

LONG joytest_main(const char *args, ULONG arglen)
{
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);
    int once = word(args, arglen, "once"), both = word(args, arglen, "mouseport");
    ULONG got = 0;
    pot = OpenResource("potgo.resource");
    if (pot) {
        got = AllocPotBits(0xF000);                   /* the joystick port's pins 5 and 9 */
        if (got) WritePotgo(0x0000, got);             /* inputs: a pad's buttons pull them low */
    }
    if (got != 0xF000) puts_("(the joystick port's pins 5 and 9 are in use by another program: FIRE2/FIRE3 may not show)\n");
    if (!once) puts_(both ? "Both ports (a mouse moving in the mouse port shows as directions); Ctrl-C to stop.\n" : "The joystick port: move the stick and press the buttons; Ctrl-C to stop.\n");
    ULONG last = 0xffffffffu;
    for (;;) {
        UWORD j1 = *JOY1DAT, j0 = *JOY0DAT, pg = *POTGOR;
        UBYTE pra = *CIAAPRA;
        ULONG p2 = port_state(j1, !(pra & 0x80), !(pg & 0x4000), !(pg & 0x1000));
        ULONG p1 = port_state(j0, !(pra & 0x40), !(pg & 0x0400), 0);
        ULONG now = p2 | (both ? p1 << 8 : 0);        /* the mouse's counters only when asked: they move with it */
        if (now != last || once) {
            show_port("Joystick port: ", p2, 1);
            if (both) show_port("  Mouse port: ", p1, 0);
            puts_(" (JOY1DAT $"); hex4(j1);
            if (both) { puts_(" JOY0DAT $"); hex4(j0); }
            puts_(")\n");
            last = now;
        }
        if (once) break;
        if (brk()) { puts_("*** Break\n"); break; }
        Delay(dos, 1);
    }
    if (got) FreePotBits(got);
    CloseLibrary(dos);
    return 0;
}
