/* agaboot USBMOUSE, USBPAD, USBFIND - the Pi's own USB mouse and pad in WHDLoad games
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The mouse and the pad plugged into the Pi 4's or CM4's own USB belong to
 * Poseidon and the user's xhci.device. In a WHDLoad game they stop with the
 * OS, and the kernel (arm/aga_usb.c) borrows the devices' endpoints for the
 * game's length - the mouse for port 0, the pad for port 1. It has to be told
 * which endpoints, and where in a report the buttons and the rest are.
 * Getting that wrong is how you end up with a pad that fires on its own.
 *
 * By itself that comes from the USB stack (usbfind.c): the first mouse and
 * the first pad Poseidon has, each laid out from its own report descriptor,
 * found by where it is plugged in. agaboot USBFIND shows what it finds. A
 * device it gets wrong can be set by hand instead:
 *
 *   agaboot USBMOUSE ADDR=2 EP=5 ID=2 BUTTONS=8,16 X=24,16 Y=40,16 [DIV=4] [LOG] [SAVE]
 *   agaboot USBPAD ADDR=3 EP=3 ID=1 HAT=40,4 X=8,8 Y=16,8 RED=45 BLUE=46 GREEN=47
 *                  YELLOW=44 REVERSE=48 FORWARD=49 PLAY=53 [LOG] [SAVE]
 *
 * ADDR the device's USB address on the xHCI, as RAM:usbprobe shows it - or
 * PORT= and ROUTE= where it is plugged in, the root port and the hub ports
 * below it (hex, the first hub's in the last digit), as USBFIND shows them;
 * EP the DCI of its interrupt IN endpoint; ID its report ID (0 none); the
 * fields the first bit and the number of bits, counted from the report's
 * first byte, the ID included. The mouse: BUTTONS, X and Y; DIV how many of
 * the mouse's counts make one of the Amiga's (a 1000 dpi mouse: 4 or 5). The
 * pad: HAT the hat switch (0 up, clockwise to 7, anything else centred; 4
 * bits if no number follows; a third number, the value for up, if not 0), X
 * and Y a stick (its outer quarters are the directions: of all its bits
 * unsigned, or of the range two more numbers give), either or both; RED to
 * PLAY the bit of each CD32 button, the ones left out none - as the A314's
 * a314pad maps an F710, so the Controller window's names mean the same
 * buttons: A red, B blue, X yellow, Y green, LB reverse, RB forward, Start
 * play (the example above, an F710 in D mode). LOG puts reports into the
 * kernel's diag log (agastat LOG) to check the layout: the mouse's first
 * ones, every one of the pad's that changes. USBMOUSE DIV=n alone keeps the
 * found mouse with that divisor, USBPAD LOG alone the found pad, logged.
 * AGA_USBMOUSE and AGA_USBPAD in ENV: (and ENVARC: with SAVE). USBMOUSE OFF
 * and USBPAD OFF forget them: the found ones again.
 *
 * Sent at each game's entry (usbm_entry): a block in fast RAM, its address
 * in $DFF1EC/$DFF1EE, then $DFF1F2 = $5BA1 (the mouse) or $5BA3 (the pad) -
 * the kernel copies it at once - or $5BA0 / $5BA2 when there is none, or when
 * the Controller window's Controllers from leaves the PiStorm USB out (no
 * USB=ON in AGA_PAD, pad.c). While that window is open on the PiStorm USB,
 * $5BA5 has the kernel read the pad's reports for its tester - the pad stays
 * Poseidon's - and $5BA4 stops it (usbm_tester). A kernel without them
 * ignores them; one without the route (flags bit 1) finds no device at
 * address 0. Nothing to do but match the version and curse.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "usbmouse.h"
#include "usbfind.h"

#define AGA_CTRL   (*(volatile UWORD *)0xDFF1F2)
#define AGA_ADDRHI (*(volatile UWORD *)0xDFF1EC)
#define AGA_ADDRLO (*(volatile UWORD *)0xDFF1EE)
#define VAR        "AGA_USBMOUSE"
#define PADVAR     "AGA_USBPAD"

struct usbm {
    int addr, ep, id, div, log;
    int b_off, b_n, x_off, x_bits, y_off, y_bits;
    int where, root, route;                        /* found by where it is plugged in, not by ADDR - ADDR is for the desperate */
};

/* the pad's CD32 buttons, in the kernel block's order. Order matters. */
static const char *const btn_name[7] = { "RED", "BLUE", "GREEN", "YELLOW", "FORWARD", "REVERSE", "PLAY" };

struct usbp {
    int addr, ep, id, log;
    int hat_off, hat_bits, x_off, x_bits, y_off, y_bits;
    int btn[7];                                    /* -1 means none */
    int where, root, route;                        /* same as the mouse's */
    int ranges, hat_min, x_min, x_max, y_min, y_max;   /* the device's logical values, when known - when the descriptor bothered */
};

/* "8,16" or "8,16,0,255": up to max numbers; how many, 0 if it's rubbish */
static int nums(const char *s, int *v, int max)
{
    int n = 0;
    while (n < max) {
        char *e;
        long x = strtol(s, &e, 10);
        if (e == s) return 0;
        v[n++] = (int)x;
        if (*e != ',') return *e ? 0 : n;
        s = e + 1;
    }
    return 0;
}

static int num(const char *s, int *a)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s) return 0;
    *a = (int)v;
    return 1;
}

static int hex(const char *s, int *a)
{
    char *e;
    long v = strtol(s, &e, 16);
    if (e == s || *e) return 0;
    *a = (int)v;
    return 1;
}

/* PORT= and ROUTE=, the same for both: 1 taken, 0 not these, -1 a bad value */
static int where_word(const char *w, int *where, int *root, int *route)
{
    if (!strnicmp(w, "PORT=", 5)) { *where = 1; return num(w + 5, root) && *root >= 1 && *root <= 255 ? 1 : -1; }
    if (!strnicmp(w, "ROUTE=", 6)) return hex(w + 6, route) && !(*route & ~0xFFFFF) ? 1 : -1;
    return 0;
}

/* one word into the setting: 1 taken, 0 not one of mine, -1 a bad value */
static int word(struct usbm *u, const char *w)
{
    int v[2], r = where_word(w, &u->where, &u->root, &u->route);
    if (r) return r;
    if (!strnicmp(w, "ADDR=", 5)) return num(w + 5, &u->addr) && u->addr >= 1 && u->addr <= 127 ? 1 : -1;
    if (!strnicmp(w, "EP=", 3)) return num(w + 3, &u->ep) && u->ep >= 3 && u->ep <= 31 && (u->ep & 1) ? 1 : -1;
    if (!strnicmp(w, "ID=", 3)) return num(w + 3, &u->id) && u->id >= 0 && u->id <= 255 ? 1 : -1;
    if (!strnicmp(w, "DIV=", 4)) return num(w + 4, &u->div) && u->div >= 1 && u->div <= 64 ? 1 : -1;
    if (!strnicmp(w, "BUTTONS=", 8)) {
        if (nums(w + 8, v, 2) != 2) return -1;
        u->b_off = v[0]; u->b_n = v[1];
        return u->b_n >= 1 && u->b_n <= 16 ? 1 : -1;
    }
    if (!strnicmp(w, "X=", 2) || !strnicmp(w, "Y=", 2)) {
        int *off = w[0] == 'X' || w[0] == 'x' ? &u->x_off : &u->y_off, *bits = off == &u->x_off ? &u->x_bits : &u->y_bits;
        if (nums(w + 2, v, 2) != 2) return -1;
        *off = v[0]; *bits = v[1];
        return *bits >= 2 && *bits <= 16 ? 1 : -1;
    }
    if (!stricmp(w, "LOG")) { u->log = 1; return 1; }
    return 0;
}

static int complete(const struct usbm *u)
{
    return (u->addr || u->where) && u->ep && u->b_n && u->x_bits && u->y_bits
        && u->b_off >= 0 && u->x_off >= 0 && u->y_off >= 0
        && u->b_off + u->b_n <= 512 && u->x_off + u->x_bits <= 512 && u->y_off + u->y_bits <= 512;
}

static char *where_text(char *s, int where, int root, int route, int addr)
{
    return s + (where ? sprintf(s, "PORT=%d ROUTE=%x", root, route) : sprintf(s, "ADDR=%d", addr));
}

static void format(const struct usbm *u, char *s)
{
    s = where_text(s, u->where, u->root, u->route, u->addr);
    sprintf(s, " EP=%d ID=%d BUTTONS=%d,%d X=%d,%d Y=%d,%d DIV=%d%s", u->ep, u->id,
            u->b_off, u->b_n, u->x_off, u->x_bits, u->y_off, u->y_bits, u->div, u->log ? " LOG" : "");
}

/* and the same damn thing for the pad */
static int pword(struct usbp *u, const char *w)
{
    int v[4], n, r = where_word(w, &u->where, &u->root, &u->route);
    if (r) return r;
    if (!strnicmp(w, "ADDR=", 5)) return num(w + 5, &u->addr) && u->addr >= 1 && u->addr <= 127 ? 1 : -1;
    if (!strnicmp(w, "EP=", 3)) return num(w + 3, &u->ep) && u->ep >= 3 && u->ep <= 31 && (u->ep & 1) ? 1 : -1;
    if (!strnicmp(w, "ID=", 3)) return num(w + 3, &u->id) && u->id >= 0 && u->id <= 255 ? 1 : -1;
    if (!strnicmp(w, "HAT=", 4)) {
        if (!(n = nums(w + 4, v, 3))) return -1;
        u->hat_off = v[0];
        u->hat_bits = n >= 2 ? v[1] : 4;
        if (n == 3) { u->hat_min = v[2]; u->ranges = 1; }
        return u->hat_off >= 0 && u->hat_off <= 500 && u->hat_bits >= 3 && u->hat_bits <= 8
            && u->hat_min >= 0 && u->hat_min <= 15 ? 1 : -1;
    }
    if (!strnicmp(w, "X=", 2) || !strnicmp(w, "Y=", 2)) {
        int x = w[0] == 'X' || w[0] == 'x';
        n = nums(w + 2, v, 4);
        if (n != 2 && n != 4) return -1;
        if (x) { u->x_off = v[0]; u->x_bits = v[1]; } else { u->y_off = v[0]; u->y_bits = v[1]; }
        if (n == 4) {
            if (x) { u->x_min = v[2]; u->x_max = v[3]; } else { u->y_min = v[2]; u->y_max = v[3]; }
            u->ranges = 1;
        }
        return v[0] >= 0 && v[0] <= 500 && v[1] >= 2 && v[1] <= 16 && (n == 2 || v[2] < v[3]) ? 1 : -1;
    }
    for (int i = 0; i < 7; i++) {
        int l = (int)strlen(btn_name[i]);
        if (!strnicmp(w, btn_name[i], l) && w[l] == '=')
            return num(w + l + 1, &u->btn[i]) && u->btn[i] >= 0 && u->btn[i] < 512 ? 1 : -1;
    }
    if (!stricmp(w, "LOG")) { u->log = 1; return 1; }
    return 0;
}

static int pcomplete(const struct usbp *u)
{
    return (u->addr || u->where) && u->ep && (u->hat_bits || u->x_bits || u->y_bits);
}

static void pformat(const struct usbp *u, char *s)
{
    s = where_text(s, u->where, u->root, u->route, u->addr);
    s += sprintf(s, " EP=%d ID=%d", u->ep, u->id);
    if (u->hat_bits) s += sprintf(s, u->hat_min ? " HAT=%d,%d,%d" : " HAT=%d,%d", u->hat_off, u->hat_bits, u->hat_min);
    if (u->x_bits) s += sprintf(s, u->x_min < u->x_max ? " X=%d,%d,%d,%d" : " X=%d,%d", u->x_off, u->x_bits, u->x_min, u->x_max);
    if (u->y_bits) s += sprintf(s, u->y_min < u->y_max ? " Y=%d,%d,%d,%d" : " Y=%d,%d", u->y_off, u->y_bits, u->y_min, u->y_max);
    for (int i = 0; i < 7; i++)
        if (u->btn[i] >= 0) s += sprintf(s, " %s=%d", btn_name[i], u->btn[i]);
    if (u->log) strcpy(s, " LOG");
}

static void pclear(struct usbp *u)
{
    memset(u, 0, sizeof *u);
    for (int i = 0; i < 7; i++) u->btn[i] = -1;
}

/* a setting's words as ENV: has them, each through take(); 0 none or a bad one */
static int load_words(const char *var, void *u, int (*take)(void *, const char *))
{
    char s[240], *p = s;
    if (GetVar((STRPTR)var, (STRPTR)s, sizeof s, GVF_GLOBAL_ONLY) <= 0) return 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        char *w = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        char c = *p;
        *p = 0;
        if (*w && take(u, w) < 0) return 0;
        *p = c;
    }
    return 1;
}
static int take_m(void *u, const char *w) { return word(u, w); }
static int take_p(void *u, const char *w) { return pword(u, w); }

/* the setting as ENV: has it; 0 none (or not whole) - what it does have
   (DIV=, LOG) still in *u, because throwing that away would be rude */
static int load(struct usbm *u)
{
    memset(u, 0, sizeof *u);
    u->div = 4;
    if (load_words(VAR, u, take_m) && complete(u)) return 1;
    int div = u->div, log = u->log;
    memset(u, 0, sizeof *u);
    u->div = div; u->log = log;
    return 0;
}

static int pload(struct usbp *u)
{
    pclear(u);
    if (load_words(PADVAR, u, take_p) && pcomplete(u)) return 1;
    int log = u->log;
    pclear(u);
    u->log = log;
    return 0;
}

/* ---- found by the USB stack (usbfind.c) -------------------------------------- */

/* the CD32 buttons from the pad's own (red, blue, green, yellow, forward,
   reverse, play), numbered as a RumblePad's D mode and most PC pads number
   them: X 1, A 2, B 3, Y 4, LB 5, RB 6, Start 10 (8 on a pad with fewer); a
   stick with one to three buttons fires with the first. Mad, but it's the
   convention I have to live with. */
static const UBYTE pad_map[7] = { 2, 3, 4, 1, 6, 5, 10 };
static const UBYTE stick_map[7] = { 1, 2, 0, 0, 0, 0, 3 };

static void from_mouse(const struct usbf *f, struct usbm *u)
{
    u->where = 1; u->root = f->root; u->route = (int)f->route;
    u->ep = f->dci; u->id = f->id;
    u->b_off = f->btn[0]; u->b_n = 0;
    while (u->b_n < f->nbtn && f->btn[u->b_n] == f->btn[0] + u->b_n) u->b_n++;   /* side by side, as the kernel reads them - count until the run breaks */
    u->x_off = f->x_off; u->x_bits = f->x_bits;
    u->y_off = f->y_off; u->y_bits = f->y_bits;
}

static void from_pad(const struct usbf *f, struct usbp *p)
{
    p->where = 1; p->root = f->root; p->route = (int)f->route;
    p->ep = f->dci; p->id = f->id;
    p->hat_off = f->hat_off; p->hat_bits = f->hat_bits; p->hat_min = f->hat_min;
    p->x_off = f->x_off; p->x_bits = f->x_bits; p->x_min = (int)f->x_min; p->x_max = (int)f->x_max;
    p->y_off = f->y_off; p->y_bits = f->y_bits; p->y_min = (int)f->y_min; p->y_max = (int)f->y_max;
    p->ranges = 1;
    for (int i = 0; i < 7; i++) {
        int n = f->nbtn >= 4 ? pad_map[i] : stick_map[i];
        if (i == 6 && f->nbtn >= 4 && f->nbtn < 10) n = 8;
        p->btn[i] = n && n <= f->nbtn ? f->btn[n - 1] : -1;
    }
}

/* what is not set by hand, from the stack: u and p filled when it has one
   (either may be NULL); 0 when there is no stack to ask */
static int found(struct usbm *u, int *mu, struct usbp *p, int *mp)
{
    struct usbf fm, fp;
    if (!usbf_find(u ? &fm : NULL, p ? &fp : NULL, 0)) return 0;
    if (u && fm.found && fm.nbtn) { from_mouse(&fm, u); *mu = 1; }
    if (p && fp.found) { from_pad(&fp, p); *mp = 1; }
    return 1;
}

/* ---- to the kernel ------------------------------------------------------------ */

/* a block to the kernel: its address, then the control word that takes it -
   in that order, or it's garbage */
static void send(UBYTE *b, UWORD ctrl)
{
    CacheClearU();
    Disable();
    AGA_ADDRHI = (UWORD)((ULONG)b >> 16);
    AGA_ADDRLO = (UWORD)(ULONG)b;
    AGA_CTRL = ctrl;                               /* copied by the kernel before this returns; that's the deal */
    Enable();
}

static void put32(UBYTE *b, LONG v)
{
    b[0] = (UBYTE)(v >> 24); b[1] = (UBYTE)(v >> 16); b[2] = (UBYTE)(v >> 8); b[3] = (UBYTE)v;
}

/* the mouse's block to the kernel - 24 bytes, the last four where it is
   (flags bit 1); 0 without the memory for it */
static int send_mouse(const struct usbm *u)
{
    UBYTE *b = AllocMem(24, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) return 0;
    b[0] = 'U'; b[1] = 'S'; b[2] = 'B'; b[3] = 'M';
    b[4] = (UBYTE)u->addr; b[5] = (UBYTE)u->ep; b[6] = (UBYTE)u->id; b[7] = (UBYTE)u->div;
    b[8] = (UBYTE)(u->b_off >> 8); b[9] = (UBYTE)u->b_off; b[10] = (UBYTE)u->b_n;
    b[11] = (UBYTE)((u->log ? 1 : 0) | (u->where ? 2 : 0));
    b[12] = (UBYTE)(u->x_off >> 8); b[13] = (UBYTE)u->x_off; b[14] = (UBYTE)u->x_bits;
    b[16] = (UBYTE)(u->y_off >> 8); b[17] = (UBYTE)u->y_off; b[18] = (UBYTE)u->y_bits;
    put32(b + 20, (LONG)u->root << 24 | u->route);
    send(b, 0x5BA1);
    FreeMem(b, 24);
    return 1;
}

/* the pad's block - 56 bytes: where it is at 36 (flags bit 1), the hat's
   first value at 11 and the sticks' ranges at 40 (flags bit 2) */
static int send_pad(const struct usbp *p)
{
    UBYTE *b = AllocMem(56, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    if (!b) return 0;
    b[0] = 'U'; b[1] = 'S'; b[2] = 'B'; b[3] = 'P';
    b[4] = (UBYTE)p->addr; b[5] = (UBYTE)p->ep; b[6] = (UBYTE)p->id;
    b[7] = (UBYTE)((p->log ? 1 : 0) | (p->where ? 2 : 0) | (p->ranges ? 4 : 0));
    b[8] = (UBYTE)(p->hat_off >> 8); b[9] = (UBYTE)p->hat_off; b[10] = (UBYTE)p->hat_bits; b[11] = (UBYTE)p->hat_min;
    b[12] = (UBYTE)(p->x_off >> 8); b[13] = (UBYTE)p->x_off; b[14] = (UBYTE)p->x_bits;
    b[16] = (UBYTE)(p->y_off >> 8); b[17] = (UBYTE)p->y_off; b[18] = (UBYTE)p->y_bits;
    for (int i = 0; i < 7; i++) {
        UWORD v = p->btn[i] < 0 ? 0xFFFF : (UWORD)p->btn[i];
        b[20 + 2 * i] = (UBYTE)(v >> 8); b[21 + 2 * i] = (UBYTE)v;
    }
    put32(b + 36, (LONG)p->root << 24 | p->route);
    put32(b + 40, p->x_min); put32(b + 44, p->x_max);
    put32(b + 48, p->y_min); put32(b + 52, p->y_max);
    send(b, 0x5BA3);
    FreeMem(b, 56);
    return 1;
}

/* SANDBOX ON, before the entry: the kernel gets the mouse and the pad, or
   forgets them (use 0: both forgotten). With no USB stack to ask - Poseidon
   not started yet, at boot - it keeps what it was given last. Nothing I can
   do about that one. */
void usbm_entry(int use)
{
    struct usbm u;
    struct usbp p;
    if (!use) {
        AGA_CTRL = 0x5BA0;
        AGA_CTRL = 0x5BA2;
        return;
    }
    int hm = load(&u), hp = pload(&p), asked = 1;
    if (!hm || !hp) asked = found(hm ? NULL : &u, &hm, hp ? NULL : &p, &hp);
    if (hm ? !send_mouse(&u) : asked) AGA_CTRL = 0x5BA0;
    if (hp ? !send_pad(&p) : asked) AGA_CTRL = 0x5BA2;
}

/* The Controller window's tester (pad.c): the kernel reads the pad's
   reports while the window is open on the PiStorm USB ($5BA5), so the tester
   shows it - it stays Poseidon's - and stops when it closes ($5BA4). 0: no
   pad, nothing read. */
int usbm_tester(int on)
{
    struct usbp p;
    if (!on) {
        AGA_CTRL = 0x5BA4;
        return 0;
    }
    int hp = pload(&p);
    if (!hp) found(NULL, NULL, &p, &hp);
    if (!hp || !send_pad(&p)) return 0;
    AGA_CTRL = 0x5BA5;
    return 1;
}

/* ---- the commands ---------------------------------------------------------- */

int usbm_command(int argc, char **argv)
{
    struct usbm u;
    char s[160];
    int save = 0, off = 0, words = 0;
    for (int i = 2; i < argc; i++) {
        if (!stricmp(argv[i], "SAVE")) save = 1;
        else if (!stricmp(argv[i], "OFF")) off = 1;
        else words++;
    }
    if (off) {
        DeleteVar((STRPTR)VAR, GVF_GLOBAL_ONLY);
        if (save) DeleteVar((STRPTR)VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        PutStr(save ? "agaboot: no USB mouse set by hand, saved - games get the one found (agaboot USBFIND)\n"
                    : "agaboot: no USB mouse set by hand - games get the one found (agaboot USBFIND)\n");
        return 0;
    }
    if (!words) {
        if (load(&u)) { format(&u, s); Printf("agaboot: USB mouse in games, set by hand: %s\n", (ULONG)s); }
        else PutStr("agaboot: no USB mouse set by hand - games get the one found (agaboot USBFIND); "
                    "agaboot USBMOUSE ADDR= EP= ID= BUTTONS= X= Y= [DIV=] [LOG] [SAVE] sets one\n");
        return 0;
    }
    memset(&u, 0, sizeof u);
    u.div = 4;
    for (int i = 2; i < argc; i++) {
        if (!stricmp(argv[i], "SAVE")) continue;
        int r = word(&u, argv[i]);
        if (r <= 0) { Printf("agaboot: USBMOUSE: %s %s\n", (ULONG)argv[i], (ULONG)(r < 0 ? "is out of range" : "is not understood")); return 5; }
    }
    int only = 0;
    if (!complete(&u)) {
        only = (u.div != 4 || u.log) && !u.addr && !u.where && !u.ep && !u.b_n && !u.x_bits && !u.y_bits;
        if (!only) { PutStr("agaboot: USBMOUSE needs ADDR= (or PORT=), EP=, BUTTONS=, X= and Y=\n"); return 5; }
        sprintf(s, "DIV=%d%s", u.div, u.log ? " LOG" : "");   /* the found mouse, with these stuck on it */
    } else format(&u, s);
    SetVar((STRPTR)VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY);
    if (save) SetVar((STRPTR)VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    Printf("agaboot: USB mouse in games: %s%s%s - from the next game on\n",
           (ULONG)(only ? "the one found, " : ""), (ULONG)s, (ULONG)(save ? ", saved" : ""));
    return 0;
}

int usbp_command(int argc, char **argv)
{
    struct usbp u;
    char s[240];
    int save = 0, off = 0, words = 0;
    for (int i = 2; i < argc; i++) {
        if (!stricmp(argv[i], "SAVE")) save = 1;
        else if (!stricmp(argv[i], "OFF")) off = 1;
        else words++;
    }
    if (off) {
        DeleteVar((STRPTR)PADVAR, GVF_GLOBAL_ONLY);
        if (save) DeleteVar((STRPTR)PADVAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        PutStr(save ? "agaboot: no USB pad set by hand, saved - games get the one found (agaboot USBFIND)\n"
                    : "agaboot: no USB pad set by hand - games get the one found (agaboot USBFIND)\n");
        return 0;
    }
    if (!words) {
        if (pload(&u)) { pformat(&u, s); Printf("agaboot: USB pad in games, set by hand: %s\n", (ULONG)s); }
        else PutStr("agaboot: no USB pad set by hand - games get the one found (agaboot USBFIND); "
                    "agaboot USBPAD ADDR= EP= ID= HAT= X= Y= RED= BLUE= GREEN= YELLOW= FORWARD= REVERSE= PLAY= [LOG] [SAVE] sets one\n");
        return 0;
    }
    pclear(&u);
    for (int i = 2; i < argc; i++) {
        if (!stricmp(argv[i], "SAVE")) continue;
        int r = pword(&u, argv[i]);
        if (r <= 0) { Printf("agaboot: USBPAD: %s %s\n", (ULONG)argv[i], (ULONG)(r < 0 ? "is out of range" : "is not understood")); return 5; }
    }
    int only = !pcomplete(&u);
    if (only) {
        if (!u.log || words != 1) { PutStr("agaboot: USBPAD needs ADDR= (or PORT=), EP= and HAT= or X=/Y=\n"); return 5; }
        strcpy(s, "LOG");                          /* the found pad, logged and all */
    } else pformat(&u, s);
    SetVar((STRPTR)PADVAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY);
    if (save) SetVar((STRPTR)PADVAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    Printf("agaboot: USB pad in games: %s%s%s - from the next game on\n",
           (ULONG)(only ? "the one found, " : ""), (ULONG)s, (ULONG)(save ? ", saved" : ""));
    return 0;
}

/* agaboot USBFIND: every HID interface the stack has, and what games get -
   the found ones as USBMOUSE and USBPAD would take them, to set by hand
   when a pad's buttons come out wrong. And they will. */
int usbf_command(void)
{
    struct usbf fm, fp;
    struct usbm u;
    struct usbp p;
    char s[240];
    PutStr("agaboot: the USB stack's HID devices\n");
    if (!usbf_find(&fm, &fp, 1)) return 5;
    if (load(&u)) { format(&u, s); Printf("agaboot: games get the mouse set by hand: %s\n", (ULONG)s); }
    else if (fm.found && fm.nbtn) { from_mouse(&fm, &u); format(&u, s); Printf("agaboot: games get %s: %s\n", (ULONG)fm.name, (ULONG)s); }
    else PutStr("agaboot: no USB mouse for games\n");
    if (pload(&p)) { pformat(&p, s); Printf("agaboot: games get the pad set by hand: %s\n", (ULONG)s); }
    else if (fp.found) { from_pad(&fp, &p); pformat(&p, s); Printf("agaboot: games get %s: %s\n", (ULONG)fp.name, (ULONG)s); }
    else PutStr("agaboot: no USB pad for games\n");
    return 0;
}
