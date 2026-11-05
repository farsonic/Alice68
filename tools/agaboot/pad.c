/* agaboot - Chipset menu - Expert - Controller...: the gamepads and the mouse
 * a WHDLoad game gets through the kernel (arm/aga_pad.c) - the A314's, and the
 * ones on the PiStorm's own USB (usbmouse.c, arm/aga_usb.c) - with a tester
 * that lights each button of the A314's as it is pressed (and a mouse's
 * arrows as it moves), and the button that jumps. All of it held together
 * with ENV: variables and bad language.
 *
 * Controllers from picks where they come from: none (as it comes - games get
 * the Amiga's own ports), the A314, the PiStorm's USB, or both. With the
 * PiStorm's USB the kernel reads the USB pad's reports while the window is
 * open (usbm_tester), so the tester shows it too - it stays Poseidon's, the
 * kernel only looks at what Poseidon's driver gets. The jump
 * button and the pad modes are the same for both: the kernel adds a USB pad to
 * the joystick port before it applies them.
 *
 * Most Amiga games jump with the stick pushed up, awkward on a pad's d-pad.
 * The jump button gives the game Up instead of itself, and Up on the pad
 * still jumps too. Some games jump with a button instead (Turrican 2 AGA in
 * its CD32 mode, on A): "Game jumps with" names it, and then the jump button
 * and Up on the pad both press it - Up still going up as well (for the games
 * that climb with it), or, at Up only jumps, not any more - or, at Up stays
 * Up, only the jump button presses it: for the games whose menus pick with
 * that button, where Up pressing it too would pick instead of move.
 *
 * A pad is a CD32 pad to the game, as on a real A1200 with one, and a game
 * that looks for one switches to its CD32 controls. Beside the jump button it
 * can be a 2-button stick instead (A on fire, B the second button) or a
 * 1-button one (A alone): for the games whose CD32 controls are worse than
 * their joystick ones, or that a second button upsets.
 *
 * D-PAD Jump is that setting kept once, for no game in particular: as it
 * comes, RB and Up press A (Turrican 2 AGA's), edited and saved like any
 * other. Switched on (On in all games), every game without its own gets it,
 * and it stays so; switched off, games get Up as a joystick's and D-PAD Jump
 * keeps what it was. A game's own always wins:
 *
 *   ENVARC:AGA_PAD                    DPAD=ON: D-PAD Jump on in all games (off
 *                                     when not set)
 *                                     JUMP=UP|A|B|X|Y|LB|RB|START: D-PAD Jump's
 *                                     jump button (UP: none, Up jumps)
 *                                     AS=A|B|X|Y|LB|RB|START: ...the button the
 *                                     game jumps with (with Up when not set)
 *                                     UP=ONLY: ...and Up on the pad gives no Up;
 *                                     UP=PLAIN: ...Up stays just Up, only the jump
 *                                     button presses it
 *                                     MODE=JOY2|JOY1: ...a pad is a 2-button or a
 *                                     1-button stick to the game (CD32: a CD32
 *                                     pad, as when not set)
 *                                     (no JUMP= at all: D-PAD Jump as it comes)
 *                                     A314=ON: the A314's pads and mouse in games
 *                                     (not used when not set)
 *                                     USB=ON: the PiStorm's USB pad and mouse in
 *                                     games (not used when not set)
 *   ENVARC:AGA-PISTORM/Pads/<drawer>  JUMP=, AS=, UP=, MODE= for one game: the drawer
 *                                     WHDLoad starts it in, as for its own picture
 *
 * Controllers from without the A314 only stops games getting its pads:
 * A314Pad, the A314's Pi and the block go on as they are, and the kernel still
 * keeps the block and a314.device's ComArea at a game's end (arm/aga_pad.c), so
 * the A314's link is never cut by it. Without the PiStorm USB, the kernel
 * forgets the USB mouse and pad at the entry (usbm_entry) and borrows nothing
 * from Poseidon.
 *
 * The window's Settings for has D-PAD Jump, the last game started and every
 * game with its own - so a game's own can be changed after a power cycle too,
 * before the game has been started again. That saved my arse more than once.
 *
 * A game has its own only while it differs from what all games get. The names
 * are the F710's as the A314's a314pad maps them, a CD32 pad's colours beside
 * them: A red, B blue, X yellow, Y green, LB reverse, RB forward, Start play.
 *
 * Sent at each game's entry (pad_entry) as $DFF1F2 = $5B50 + n, n = bit 0 |
 * bits 2-3 >> 1 (bit 1 clear: sound.c says why), $5B60 + the game's button
 * the same way (+ $10 for UP=ONLY; $5B80 + it for UP=PLAIN), $5B90 + the mode
 * (0 CD32 pad, 1 2-button stick, 2 1-button) and $5B49 / $5B48 for the A314
 * used or not. A stock Emu68 ignores them, which is its loss.
 *
 * The tester reads the block the A314's Pi writes (aga_pad.c) straight from
 * chip RAM - the kernel says where (diag selector $1805) - and shows the
 * directions as the game gets them, the jump button's Up included.
 *
 *   agaboot PAD                       what is set, and whether the pads are live
 *   agaboot PAD JUMP=<button> [AS=<button>] [UP=ALSO|ONLY|PLAIN] [MODE=CD32|JOY2|JOY1] [SAVE]
 *                                     D-PAD Jump
 *   agaboot PAD DPAD=ON|OFF [SAVE]    D-PAD Jump in all games, or not
 *   agaboot PAD A314=ON|OFF [SAVE]    the A314's pads and mouse in games, or not
 *   agaboot PAD USB=ON|OFF [SAVE]     the PiStorm's USB pad and mouse in games, or not
 *   agaboot PAD GAME=<drawer> JUMP= AS= UP= MODE= [SAVE] | REMOVE
 *   agaboot PAD ASK                   the window
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <dos/dosextens.h>                         /* FileInfoBlock, DOS_FIB: the games with their own, if any survive */
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <string.h>
#include <stdio.h>
#include "pad.h"
#include "picture.h"                               /* LASTGAME_VAR, the one thing I need from there */
#include "front.h"
#include "usbmouse.h"

#define AGA_CTRL   (*(volatile UWORD *)0xDFF1F2)
#define DIAG_SEL_W (*(volatile UWORD *)0xDFF1E8)
#define DIAG_DAT_W (*(volatile UWORD *)0xDFF1EA)
#define SEL_PAD    0x1805                          /* arm/aga_diag.c - the selector lives in there */

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *GadToolsBase;               /* picture.c - its GadToolsBase */
void kernel_boards(int *built, int *runs);         /* agaboot.c, the bugger that calls me */

#define PAD_VAR  "AGA_PAD"
#define PADS_DIR "AGA-PISTORM/Pads/"
#define MAX_GAMES 24                               /* games in the window's Settings for - cap it, memory isn't free */
#define PAD_MAGIC 0x41335044UL                     /* 'A3PD' - same magic as the kernel's block */

static const char *const jump_words[8] = { "UP", "A", "B", "X", "Y", "LB", "RB", "START" };
static const char *const jump_names[] = { "None (Up jumps)", "A (red)", "B (blue)", "X (yellow)", "Y (green)",
                                          "LB (reverse)", "RB (forward)", "Start (play)", NULL };
static const char *const as_names[] = { "Up (as a joystick)", "A (red)", "B (blue)", "X (yellow)", "Y (green)",
                                        "LB (reverse)", "RB (forward)", "Start (play)", NULL };
/* beside Game jumps with: what Up on the pad does while the game jumps with
   a button. Three choices, all of them trouble */
static const char *const up_names[] = { "Up jumps too", "Up only jumps", "Up stays Up", NULL };
static const char *const up_words[3] = { "ALSO", "ONLY", "PLAIN" };
/* beside Jump button: what a pad is to the game, before it complains */
static const char *const mode_names[] = { "CD32 pad", "2-button stick", "1-button stick", NULL };
static const char *const mode_words[3] = { "CD32", "JOY2", "JOY1" };
#define DPAD_NAME  "D-PAD Jump"
#define DPAD_LABEL "On in all games"                /* beside Settings for: D-PAD Jump switched on for the lot */
#define DPAD_DEFAULT (6 | 1 << 4)                  /* D-PAD Jump as it comes: RB and Up press A, Turrican 2 AGA style */
/* the JPF bit each one sits in, in the pad's word */
static const UBYTE jump_bit[8] = { 0, 22, 23, 21, 20, 18, 19, 17 };

int pad_parse_jump(const char *w)
{
    for (int i = 0; i < 8; i++) if (!stricmp(w, jump_words[i])) return i;
    if (!stricmp(w, "NONE") || !stricmp(w, "OFF")) return 0;
    return -1;
}
const char *pad_jump_word(int n) { return jump_words[n & 7]; }
const char *pad_jump_name(int n) { return jump_names[n & 7]; }
static int parse_mode(const char *w)
{
    for (int i = 0; i < 3; i++) if (!stricmp(w, mode_words[i])) return i;
    return -1;
}

/* Up on the pad while the game jumps with a button: 0 it presses the button
   too, 1 only the button, 2 just Up (only the jump button presses it). Keep
   those three straight or the menus go wrong. */
static int up_mode(int s) { return (s & SET_ONLY) ? 1 : (s & SET_PLAIN) ? 2 : 0; }
static int with_up(int s, int m) { s &= ~(SET_ONLY | SET_PLAIN); return m == 1 ? s | SET_ONLY : m == 2 ? s | SET_PLAIN : s; }

/* nothing to do with Up while the game jumps with Up - leave it be */
static int set_norm(int s)
{
    s &= SET_JUMP_M | SET_AS_M | SET_ONLY | SET_PLAIN | SET_MODE_M;
    if (SET_MODE(s) == 3) s &= ~SET_MODE_M;
    if (!SET_AS(s)) return s & ~(SET_ONLY | SET_PLAIN);
    return (s & SET_PLAIN) ? s & ~SET_ONLY : s;
}

/* "A (red), Up presses it too" - that's the shape of it */
static const char *as_text(int s)
{
    static const char *const up_text[3] = { ", Up presses it too", ", Up presses only it", ", Up stays just Up" };
    static char t[48];
    strcpy(t, as_names[SET_AS(s)]);
    if (SET_AS(s)) strcat(t, up_text[up_mode(s)]);
    return t;
}

/* ---- settings, in ENV: where everything rots -------------------------------- */
static void game_var(const char *game, char *name)
{
    strcpy(name, PADS_DIR);
    strncat(name, game, 30);
}

/* KEY=<value> in a variable's words: the value into w, 1; 0 if it's not there */
static int var_word(const char *name, const char *key, char *w, int n)
{
    char s[64];
    int kl = strlen(key);
    if (GetVar((STRPTR)name, (STRPTR)s, sizeof s, GVF_GLOBAL_ONLY) <= 0) return 0;
    for (const char *p = s; *p; ) {
        while (*p == ' ') p++;
        if (!strnicmp(p, key, kl)) {
            int i = 0;
            for (p += kl; *p && *p != ' ' && i < n - 1; ) w[i++] = *p++;
            w[i] = 0;
            return 1;
        }
        while (*p && *p != ' ') p++;
    }
    return 0;
}

/* JUMP=, AS=, UP= and MODE= in a variable: the setting, -1 if none of them are */
static int read_var(const char *name)
{
    char w[8];
    int s = -1, v;
    if (var_word(name, "JUMP=", w, sizeof w) && (v = pad_parse_jump(w)) >= 0) s = v;
    if (var_word(name, "AS=", w, sizeof w) && (v = pad_parse_jump(w)) >= 0) s = (s < 0 ? 0 : s) | v << 4;
    if (s >= 0 && var_word(name, "UP=", w, sizeof w))
        for (int m = 1; m < 3; m++) if (!stricmp(w, up_words[m])) s = with_up(s, m);
    if (var_word(name, "MODE=", w, sizeof w) && (v = parse_mode(w)) >= 0) s = (s < 0 ? 0 : s) | v << 9;
    return s < 0 ? -1 : set_norm(s);
}

/* a setting as its variable's words: "JUMP=RB AS=A UP=PLAIN MODE=JOY2" */
static void set_words(char *t, int s)
{
    sprintf(t, "JUMP=%s", jump_words[SET_JUMP(s)]);
    if (SET_AS(s)) sprintf(t + strlen(t), " AS=%s", jump_words[SET_AS(s)]);
    if (SET_AS(s) && up_mode(s)) sprintf(t + strlen(t), " UP=%s", up_words[up_mode(s)]);
    if (SET_MODE(s)) sprintf(t + strlen(t), " MODE=%s", mode_words[SET_MODE(s)]);
}

int pad_use_a314(void)
{
    char w[8];
    return var_word(PAD_VAR, "A314=", w, sizeof w) && !stricmp(w, "ON");
}

int pad_use_usb(void)
{
    char w[8];
    return var_word(PAD_VAR, "USB=", w, sizeof w) && !stricmp(w, "ON");
}

/* D-PAD Jump: as saved, or as it comes out of the box */
int pad_preset(void)
{
    int s = read_var(PAD_VAR);
    return s >= 0 ? s : DPAD_DEFAULT;
}

int pad_dpad_on(void)
{
    char w[8];
    return var_word(PAD_VAR, "DPAD=", w, sizeof w) && !stricmp(w, "ON");
}

/* what a game without its own gets - usually nothing */
static int all_games(void) { return pad_dpad_on() ? pad_preset() : 0; }

int pad_has_own(const char *game)
{
    char name[64];
    if (!game || !game[0]) return 0;
    game_var(game, name);
    return read_var(name) >= 0;
}

int pad_jump_for(const char *game)
{
    char name[64];
    int n;
    if (game && game[0]) {
        game_var(game, name);
        if ((n = read_var(name)) >= 0) return n;
    }
    return all_games();
}

static void make_drawer(const char *path)
{
    BPTR l = CreateDir((STRPTR)path);
    if (l) UnLock(l);
}

static void set_var(const char *name, int n, int save)
{
    char s[48];
    set_words(s, n);
    SetVar((STRPTR)name, (STRPTR)s, -1, GVF_GLOBAL_ONLY);
    if (save) SetVar((STRPTR)name, (STRPTR)s, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

void pad_forget(const char *game)
{
    char name[64];
    if (!game || !game[0]) return;
    game_var(game, name);
    DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY);
    DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* all games' settings - D-PAD Jump, whether it is on, the A314, the USB - in
   one variable; none at all while they are the defaults (D-PAD Jump off and as
   it comes, neither the A314 nor the USB used). One variable, one place to
   look. */
static void store_all(int preset, int on, int a314, int usb, int save)
{
    char s[72];
    preset = set_norm(preset);
    if (preset == DPAD_DEFAULT && !on && !a314 && !usb) {
        DeleteVar((STRPTR)PAD_VAR, GVF_GLOBAL_ONLY);
        if (save) DeleteVar((STRPTR)PAD_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        return;
    }
    strcpy(s, on ? "DPAD=ON " : "");
    set_words(s + strlen(s), preset);
    if (a314) strcat(s, " A314=ON");
    if (usb) strcat(s, " USB=ON");
    SetVar((STRPTR)PAD_VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY);
    if (save) SetVar((STRPTR)PAD_VAR, (STRPTR)s, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

void pad_store_a314(int on, int save) { store_all(pad_preset(), pad_dpad_on(), on, pad_use_usb(), save); }
void pad_store_usb(int on, int save) { store_all(pad_preset(), pad_dpad_on(), pad_use_a314(), on, save); }
void pad_store_dpad(int on, int save) { store_all(pad_preset(), on, pad_use_a314(), pad_use_usb(), save); }

void pad_store(const char *game, int n, int save)
{
    char name[64];
    if (!game || !game[0]) { store_all(n, pad_dpad_on(), pad_use_a314(), pad_use_usb(), save); return; }
    /* one game: its own only while it differs from what all games get -
   otherwise forget it */
    if (n == all_games()) { pad_forget(game); return; }
    make_drawer("ENV:AGA-PISTORM");
    make_drawer("ENV:AGA-PISTORM/Pads");
    if (save) {
        make_drawer("ENVARC:AGA-PISTORM");
        make_drawer("ENVARC:AGA-PISTORM/Pads");
    }
    game_var(game, name);
    set_var(name, n, save);
}

void pad_send(int s)
{
    int n = SET_JUMP(s), t = SET_AS(s), m = SET_MODE(s);
    AGA_CTRL = (UWORD)(0x5B50 | (n & 1) | ((n & 6) << 1));
    if (s & SET_PLAIN) AGA_CTRL = (UWORD)(0x5B80 | (t & 1) | ((t & 6) << 1));
    else AGA_CTRL = (UWORD)(0x5B60 | ((s & SET_ONLY) ? 0x10 : 0) | (t & 1) | ((t & 6) << 1));
    AGA_CTRL = (UWORD)(0x5B90 | (m & 1) | ((m & 6) << 1));
}

void pad_send_a314(int on)
{
    AGA_CTRL = (UWORD)(0x5B48 | (on ? 1 : 0));
}

void pad_entry(const char *game)
{
    pad_send_a314(pad_use_a314());
    pad_send(pad_jump_for(game));
    usbm_entry(pad_use_usb());                                  /* the Pi's own USB mouse and pad (usbmouse.c), not the A314's lot */
}

/* ---- the kernel's view: where the block is, what the jump button does -------- */
struct pad_state {
    ULONG addr, jump, ca, a314, as, mode, usb;     /* SEL_PAD's seven longs; an older kernel gives as, mode, usb 0 */
    ULONG word[2];                                 /* ports 0 and 1, when they're live */
    int st;                                        /* S_*, the states below */
};
enum { S_NOKERNEL, S_NOBLOCK, S_WAIT, S_QUIET, S_LIVE };

static int our_kernel(void)
{
    int built, runs;
    kernel_boards(&built, &runs);
    return built != 0;
}

static void kernel_pad(struct pad_state *p)
{
    ULONG v[7];
    Disable();
    DIAG_SEL_W = SEL_PAD;
    for (int i = 0; i < 7; i++) {
        ULONG hi = DIAG_DAT_W;
        ULONG lo = DIAG_DAT_W;
        v[i] = (hi << 16) | lo;
    }
    Enable();
    p->addr = v[0];
    p->jump = v[1];
    p->ca = v[2];
    p->a314 = v[3];
    p->as = v[4];
    p->mode = v[5];
    p->usb = v[6];
}

/* the block's magic, beat and the two ports' words, read where the kernel
   said; an address it accepted is long-aligned chip or slow RAM. Trust it or
   don't bother reading. */
static int read_block(ULONG addr, ULONG *beat, ULONG word[2])
{
    volatile ULONG *b = (volatile ULONG *)addr;
    if (!addr || b[0] != PAD_MAGIC) return 0;
    *beat = b[1];
    word[0] = b[2];
    word[1] = b[3];
    return 1;
}

static int pad_type_ok(ULONG w) { ULONG t = w >> 28; return t == 1 || t == 2 || t == 3; }   /* a pad, a mouse, a stick - nothing else gets through */

static const char *state_text(const struct pad_state *p)
{
    switch (p->st) {
    case S_NOKERNEL: return "The AGA-PISTORM kernel is not running here.";
    case S_NOBLOCK:  return "No pads: A314Pad is not running on this Amiga.";
    case S_WAIT:     return "Waiting for the A314's Pi...";
    case S_QUIET:    return "The A314's Pi does not write the pads (a314pad?).";
    }
    if (!pad_type_ok(p->word[0]) && !pad_type_ok(p->word[1]))
        return "The A314's Pi is there, but no pad or mouse.";
    return "Live: press the buttons, move the mouse.";
}

/* ---- the window, where the real work is ------------------------------------- */
enum { G_SRC = 1, G_FOR, G_DPAD, G_JUMP, G_MODE, G_AS, G_UP, G_HINT, G_STATE, G_SAVE, G_USE, G_CANCEL };
/* Controllers from: none (as it comes), the A314, the PiStorm's USB, both -
   bit 0 the A314, bit 1 the USB. Four choices, in that order. */
static const char *const src_names[] = { "None", "A314", "PiStorm USB", "A314 + PiStorm USB", NULL };
#define OFF_TEXT   "Not used: games get the Amiga's own ports."
#define USB_TEXT   "The PiStorm's USB pad and mouse play in games."
#define USB_LIVE   "The USB pad is read in this window: press its buttons."
#define USB_NOPEEK "The USB pad could not be read (agastat LOG says why)."

/* the tester's boxes, per port: Up Down Left Right, A B X Y LB RB Start, in
   that order. The arrays below have to match or the lights lie. */
#define N_BOX 11
static const char *const box_label[N_BOX] = { "^", "v", "<", ">", "A", "B", "X", "Y", "LB", "RB", "Start" };
static const ULONG box_bit[N_BOX] = { 0x8, 0x4, 0x2, 0x1, 1UL << 22, 1UL << 23, 1UL << 21, 1UL << 20,
                                      1UL << 18, 1UL << 19, 1UL << 17 };
/* a mouse in the port: the arrows light as it moves, its buttons where A, B
   and Start are (left on red, right on blue, middle on play) */
static const char *const mouse_label[N_BOX] = { "^", "v", "<", ">", "L", "R", "", "", "", "", "Mid" };
static const ULONG mouse_bit[N_BOX] = { 0, 0, 0, 0, 1UL << 22, 1UL << 23, 0, 0, 0, 0, 1UL << 17 };
struct pbox { WORD x, y, w, h; };

struct padw {
    struct front *fr;                              /* one Controller window, and all its baggage */
    struct Screen *scr;
    struct DrawInfo *dri;
    APTR vi;
    struct Gadget *glist, *g_src, *g_for, *g_dpad, *g_jump, *g_mode, *g_as, *g_up, *g_hint, *g_state;
    int a314, usb;                                 /* Controllers from - both bits */
    int dpad;                                      /* D-PAD Jump on in all games, or off everywhere */
    struct Window *win;
    struct pbox box[2][N_BOX];
    WORD port_x, port_y[2];                        /* where the ports' names go */
    UBYTE lit[2][N_BOX];
    UBYTE mouse[2];                                /* the port is showing a mouse */
    UBYTE held[2][4];                              /* ticks an arrow stays lit after a move, so it's visible */
    UBYTE last_x[2], last_y[2];                    /* its counters a tick ago, for the delta */
    const char *for_names[MAX_GAMES + 2];
    char game[MAX_GAMES + 1][32], hint[80];        /* from 1: the last game, then the others with their own, in order */
    int ngame;                                     /* ...and how many there are */
    int fixed;                                     /* 1: game 1 is the last game, kept first no matter what */
    int sel;                                       /* 0 D-PAD Jump, 1.. a game in the list */
    int jump[MAX_GAMES + 1];                       /* D-PAD Jump's, each game's: settings as pad.h packs them. Don't invent
   another packing, pad.h is bad enough. */
    UBYTE own[MAX_GAMES + 1];                      /* the game had its own when the window opened */
    UBYTE touched[MAX_GAMES + 1];                  /* ...or I changed its setting here */
    UBYTE own_note;                                /* D-PAD Jump's hint names the games with their own, while it lasts */
    UBYTE peek;                                    /* the kernel reads the USB pad for the tester (usbm_tester), nothing more */
    struct pad_state ps;
    const char *shown;                             /* the state line as last shown */
};

static int tlen(struct RastPort *rp, const char *s) { return TextLength(rp, (STRPTR)s, strlen(s)); }

static void draw_box(struct padw *w, int port, int i, int on)
{
    struct RastPort *rp = w->win->RPort;
    const struct pbox *b = &w->box[port][i];
    const char *label = w->mouse[port] ? mouse_label[i] : box_label[i];
    UWORD *pen = w->dri->dri_Pens;
    SetAPen(rp, on ? pen[FILLPEN] : pen[BACKGROUNDPEN]);
    RectFill(rp, b->x + 2, b->y + 1, b->x + b->w - 3, b->y + b->h - 2);
    SetAPen(rp, on ? pen[FILLTEXTPEN] : pen[TEXTPEN]);
    SetDrMd(rp, JAM1);
    Move(rp, b->x + (b->w - tlen(rp, label)) / 2, b->y + (b->h - rp->TxHeight) / 2 + rp->TxBaseline);
    Text(rp, (STRPTR)label, strlen(label));
    w->lit[port][i] = (UBYTE)on;
}

static void draw_tester(struct padw *w)
{
    static const char *const port_name[2] = { "Mouse port", "Joystick port" };
    struct RastPort *rp = w->win->RPort;
    SetAPen(rp, w->dri->dri_Pens[TEXTPEN]);
    SetDrMd(rp, JAM1);
    for (int port = 1; port >= 0; port--) {
        Move(rp, w->port_x, w->port_y[port] + rp->TxBaseline);
        Text(rp, (STRPTR)port_name[port], strlen(port_name[port]));
        for (int i = 0; i < N_BOX; i++) {
            const struct pbox *b = &w->box[port][i];
            DrawBevelBox(rp, b->x, b->y, b->w, b->h, GT_VisualInfo, (ULONG)w->vi, GTBB_Recessed, TRUE, TAG_DONE);
            draw_box(w, port, i, w->lit[port][i]);
        }
    }
}

/* the jump line sits under its cycles */
/* D-PAD Jump shown, the games with their own win over it: say so, until
   D-PAD Jump is changed here - after a power cycle the window opens on it
   and nothing else would tell that a game plays otherwise (10-31) */
static int own_text(const struct padw *w, char *t)
{
    int n = 0, first = 0;
    for (int i = 1; i <= w->ngame; i++)
        if (w->own[i] || w->touched[i]) { if (!n) first = i; n++; }
    if (!n) return 0;
    if (n > 1) sprintf(t, "%d games have their own: pick one in Settings for.", n);
    else if (SET_MODE(w->jump[first])) sprintf(t, "%s has its own: a %s.", w->game[first], mode_names[SET_MODE(w->jump[first])]);
    else sprintf(t, "%s has its own jump settings.", w->game[first]);
    return 1;
}

static void set_hint(struct padw *w)
{
    int s = w->jump[w->sel], n = SET_JUMP(s), t = SET_AS(s), m = SET_MODE(s);
    if (!w->sel && w->own_note && own_text(w, w->hint)) {
        GT_SetGadgetAttrs(w->g_hint, w->win, NULL, GTTX_Text, (ULONG)w->hint, TAG_DONE);
        return;
    }
    if (m && t > (m == 1 ? 2 : 1))                 /* a stick has no X, Y, LB, RB or Start; one button has no B either */
        sprintf(w->hint, "A %s has no %s; pick %s.", mode_names[m], box_label[3 + t], m == 1 ? "A or B" : "A");
    else if (!t) {
        if (!n) strcpy(w->hint, "Up on the pad jumps, as a joystick's.");
        else sprintf(w->hint, "%s gives the game Up; Up still jumps too.", box_label[3 + n]);
    } else if (up_mode(s) == 2) {
        if (n && n != t) sprintf(w->hint, "%s presses %s; Up stays just Up.", box_label[3 + n], box_label[3 + t]);
        else sprintf(w->hint, "Up stays just Up; only %s itself jumps.", box_label[3 + t]);
    } else if (n && n != t)
        sprintf(w->hint, (s & SET_ONLY) ? "%s and Up press %s; the game gets no Up." : "%s and Up press %s; Up still goes up too.",
                box_label[3 + n], box_label[3 + t]);
    else
        sprintf(w->hint, (s & SET_ONLY) ? "Up presses %s; the game gets no Up." : "Up presses %s, and still goes up.", box_label[3 + t]);
    GT_SetGadgetAttrs(w->g_hint, w->win, NULL, GTTX_Text, (ULONG)w->hint, TAG_DONE);
}

/* the jump is the pads': off without them (Controllers from None); Up on the
   pad has nothing to choose while the game jumps with Up. Grey it out, don't
   lie about it. */
static void set_used(struct padw *w)
{
    int off = !w->a314 && !w->usb;
    GT_SetGadgetAttrs(w->g_for, w->win, NULL, GA_Disabled, off || !w->ngame, TAG_DONE);
    GT_SetGadgetAttrs(w->g_dpad, w->win, NULL, GA_Disabled, off, TAG_DONE);
    GT_SetGadgetAttrs(w->g_jump, w->win, NULL, GA_Disabled, off, TAG_DONE);
    GT_SetGadgetAttrs(w->g_mode, w->win, NULL, GA_Disabled, off, TAG_DONE);
    GT_SetGadgetAttrs(w->g_as, w->win, NULL, GA_Disabled, off, TAG_DONE);
    GT_SetGadgetAttrs(w->g_up, w->win, NULL, GA_Disabled, off || !SET_AS(w->jump[w->sel]), TAG_DONE);
}

static int src_index(const struct padw *w) { return (w->a314 ? 1 : 0) | (w->usb ? 2 : 0); }

/* the cycles as the shown setting wants them */
static void show_set(struct padw *w)
{
    int s = w->jump[w->sel];
    GT_SetGadgetAttrs(w->g_jump, w->win, NULL, GTCY_Active, SET_JUMP(s), TAG_DONE);
    GT_SetGadgetAttrs(w->g_mode, w->win, NULL, GTCY_Active, SET_MODE(s), TAG_DONE);
    GT_SetGadgetAttrs(w->g_as, w->win, NULL, GTCY_Active, SET_AS(s), TAG_DONE);
    GT_SetGadgetAttrs(w->g_up, w->win, NULL, GTCY_Active, up_mode(s), TAG_DONE);
    set_used(w);
    set_hint(w);
}

/* the pads as the game gets them: the directions as it gets them (the jump
   button's Up, Up gone at Up only jumps), the buttons as pressed and the one
   the game jumps with as pressed for it - as arm/aga_pad.c's pad(). Follow
   that again if you change one and not the other. */
static void show_pads(struct padw *w)
{
    int s = w->jump[w->sel], n = SET_JUMP(s), as = SET_AS(s);
    ULONG j = n ? 1UL << jump_bit[n] : 0, to = as ? 1UL << jump_bit[as] : 0;
    for (int port = 0; port < 2; port++) {
        /* without the A314, the game gets none of it - and the tester doesn't
           either; a USB pad is Poseidon's on Workbench, the tester never sees it */
        ULONG raw = (w->a314 && w->ps.st == S_LIVE && pad_type_ok(w->ps.word[port])) ? w->ps.word[port] : 0, dir;
        /* the USB pad on the joystick port, added as the kernel adds it in a game */
        ULONG up = (port && w->usb && w->peek && (w->ps.usb >> 28) == 1) ? w->ps.usb : 0;
        if (up && (!raw || (raw >> 28) == 1 || (raw >> 28) == 3)) raw = (1UL << 28) | ((raw | up) & 0x0FFFFFFFUL);
        dir = raw;
        int m = (raw >> 28) == 2;
        if (m != w->mouse[port]) {                 /* a mouse came or went: swap the labels before it looks silly */
            w->mouse[port] = (UBYTE)m;
            w->last_x[port] = (UBYTE)raw;
            w->last_y[port] = (UBYTE)(raw >> 8);
            for (int i = 0; i < N_BOX; i++) draw_box(w, port, i, 0);
        }
        if (m) {
            BYTE dx = (BYTE)((UBYTE)raw - w->last_x[port]), dy = (BYTE)((UBYTE)(raw >> 8) - w->last_y[port]);
            UBYTE *h = w->held[port];
            w->last_x[port] = (UBYTE)raw;
            w->last_y[port] = (UBYTE)(raw >> 8);
            if (dy < 0) h[0] = 4;
            if (dy > 0) h[1] = 4;
            if (dx < 0) h[2] = 4;
            if (dx > 0) h[3] = 4;
            for (int i = 0; i < N_BOX; i++) {
                int on = i < 4 ? h[i] > 0 : (raw & mouse_bit[i]) != 0;
                if (i < 4 && h[i]) h[i]--;
                if (on != w->lit[port][i]) draw_box(w, port, i, on);
            }
            continue;
        }
        if (!to) {
            if (dir & j) dir = (dir & ~(j | 0x4)) | 0x8;
        } else {
            ULONG up = dir & 0x8;
            if ((dir & j) || (up && up_mode(s) != 2)) dir = (dir & ~j) | to;
            if (up && up_mode(s) == 1) dir &= ~0x8UL;
        }
        for (int i = 0; i < N_BOX; i++) {
            int on = ((i < 4 ? dir : raw | dir) & box_bit[i]) != 0;
            if (on != w->lit[port][i]) draw_box(w, port, i, on);
        }
    }
    /* the A314's state while it has pads, or when nothing else does */
    int usb_live = w->usb && w->peek && (w->ps.usb >> 28) == 1;
    const char *t = w->a314 && (w->ps.st == S_LIVE || !usb_live) ? state_text(&w->ps)
                  : usb_live ? USB_LIVE : !w->usb ? OFF_TEXT : w->peek ? USB_NOPEEK : USB_TEXT;
    if (t != w->shown) {
        w->shown = t;
        GT_SetGadgetAttrs(w->g_state, w->win, NULL, GTTX_Text, (ULONG)t, TAG_DONE);
    }
}

/* one tick (50 a second): the block read; the kernel asked where it is every
   half second, as A314Pad can come and go while the window is open. Doesn't
   matter how long it's been up, ask again. */
static void tick(struct padw *w)
{
    static ULONG beat;
    static int ticks, quiet, known, kern;
    struct pad_state *p = &w->ps;
    ULONG b, word[2];
    if (ticks++ % 25 == 0) {
        ULONG was = p->addr;
        p->addr = 0;
        if ((kern = our_kernel())) kernel_pad(p);
        if (p->addr != was) known = 0;
    }
    else if (w->peek && kern) kernel_pad(p);       /* the USB pad's word, every tick while it's being read */
    if (!kern) p->st = S_NOKERNEL;
    else if (!p->addr) p->st = S_NOBLOCK;
    else if (!read_block(p->addr, &b, word)) { known = 0; p->st = S_QUIET; }
    else if (!known) { known = 1; beat = b; quiet = 0; p->st = S_WAIT; }   /* trusted once the bugger moves */
    else if (b != beat) { beat = b; quiet = 0; p->st = S_LIVE; p->word[0] = word[0]; p->word[1] = word[1]; }
    else if (++quiet > 50) p->st = S_QUIET;        /* a second without a beat and it's dead to me */
}

static void padw_close(struct padw *w)
{
    if (!w) return;
    if (w->peek) usbm_tester(0);                   /* the kernel stops reading it - tell it */
    front_release(w->fr);
    if (w->win) CloseWindow(w->win);
    if (w->glist) FreeGadgets(w->glist);
    if (w->vi) FreeVisualInfo(w->vi);
    if (w->dri) FreeScreenDrawInfo(w->scr, w->dri);
    FreeVec(w);
}

/* the games with their own in a drawer of them, each once, after those in
   the cycle already, in alphabetical order (the last game stays first) */
static void add_own_games(struct padw *w, const char *dir)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    BPTR l = Lock((STRPTR)dir, ACCESS_READ);
    if (fib && l && Examine(l, fib) && fib->fib_DirEntryType > 0) {
        while (w->ngame < MAX_GAMES && ExNext(l, fib)) {
            const char *n = (const char *)fib->fib_FileName;
            int i, at;
            if (fib->fib_DirEntryType > 0 || strlen(n) > 30 || !pad_has_own(n)) continue;
            for (i = 1; i <= w->ngame && stricmp(w->game[i], n); i++) ;
            if (i <= w->ngame) continue;           /* already in the list, skip the bastard */
            for (at = w->ngame + 1; at > w->fixed + 1 && stricmp(w->game[at - 1], n) > 0; at--)
                strcpy(w->game[at], w->game[at - 1]);
            strcpy(w->game[at], n);
            w->ngame++;
        }
    }
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
}

static struct padw *padw_open(struct Screen *scr)
{
    static const char *const buttons[] = { "Save", "Use", "Cancel", NULL };
    int other;
    struct front *fr = front_claim("AGA-PISTORM Controller", &other);
    if (other) return NULL;                        /* already open: bring the old one to the front and leave me alone */
    struct padw *w = AllocVec(sizeof *w, MEMF_ANY | MEMF_CLEAR);
    if (!w) { front_release(fr); return NULL; }
    w->fr = fr;
    w->scr = scr;
    if (GetVar((STRPTR)LASTGAME_VAR, (STRPTR)w->game[1], sizeof w->game[1], GVF_GLOBAL_ONLY) > 0 && w->game[1][0])
        w->ngame = w->fixed = 1;
    /* the games with their own: as saved, and as used since the boot */
    add_own_games(w, "ENVARC:AGA-PISTORM/Pads");
    add_own_games(w, "ENV:AGA-PISTORM/Pads");
    w->jump[0] = pad_preset();
    for (int i = 1; i <= w->ngame; i++) {
        w->jump[i] = pad_jump_for(w->game[i]);
        w->own[i] = (UBYTE)pad_has_own(w->game[i]);
    }
    w->sel = w->fixed && w->own[1];                /* the last game's own, if the poor thing has one */
    w->own_note = 1;
    w->a314 = pad_use_a314();
    w->usb = pad_use_usb();
    w->dpad = pad_dpad_on();
    w->for_names[0] = DPAD_NAME;
    for (int i = 1; i <= w->ngame; i++) w->for_names[i] = w->game[i];
    w->for_names[w->ngame + 1] = NULL;
    if (!(w->vi = GetVisualInfo(scr, TAG_DONE)) || !(w->dri = GetScreenDrawInfo(scr))) goto fail;

    struct RastPort *srp = &scr->RastPort;
    int fh = scr->Font->ta_YSize, gh = fh + 6;
    int title = scr->WBorTop + fh + 1, left = scr->WBorLeft + 10, y = title + 6;
    /* the labels' column and the cycles' widths - measure twice */
    static const char *const labels[] = { "Controllers from", "Settings for", "Jump button", "Game jumps with", NULL };
    static const char *const *const cycles[] = { jump_names, as_names, src_names, NULL };
    static const char *const hints[] = { "Start (play) gives the game Up; Up still jumps too.",
                                         "Start and Up press LB; Up still goes up too.",
                                         "Start and Up press LB; the game gets no Up.", NULL };
    int lw = 0, cw = 0, hw = 0;
    for (int i = 0; labels[i]; i++) if (tlen(srp, labels[i]) + 12 > lw) lw = tlen(srp, labels[i]) + 12;
    for (int k = 0; cycles[k]; k++)
        for (int i = 0; cycles[k][i]; i++) if (tlen(srp, cycles[k][i]) + 36 > cw) cw = tlen(srp, cycles[k][i]) + 36;
    for (int i = 0; w->for_names[i]; i++) if (tlen(srp, w->for_names[i]) + 36 > cw) cw = tlen(srp, w->for_names[i]) + 36;
    for (int i = 0; hints[i]; i++) if (tlen(srp, hints[i]) + 8 > hw) hw = tlen(srp, hints[i]) + 8;
    if (own_text(w, w->hint) && tlen(srp, w->hint) + 8 > hw) hw = tlen(srp, w->hint) + 8;
    int ucw = 0;                                   /* Up's cycle beside Game jumps with, and the mode's */
    for (int i = 0; up_names[i]; i++) if (tlen(srp, up_names[i]) + 36 > ucw) ucw = tlen(srp, up_names[i]) + 36;
    for (int i = 0; mode_names[i]; i++) if (tlen(srp, mode_names[i]) + 36 > ucw) ucw = tlen(srp, mode_names[i]) + 36;
    int xw = 26 + 8 + tlen(srp, DPAD_LABEL);       /* ...beside Jump button, and the tick box beside Settings for, of course */
    if (ucw > xw) xw = ucw;
    int iw = lw + cw + 8 + xw;
    int sw = tlen(srp, "The A314's Pi does not write the pads (a314pad?).") + 8;   /* the longest state lines decide the width, naturally */
    int ow = tlen(srp, OFF_TEXT) + 8, uw = tlen(srp, USB_TEXT) + 8;
    if (tlen(srp, USB_LIVE) + 8 > uw) uw = tlen(srp, USB_LIVE) + 8;
    if (tlen(srp, USB_NOPEEK) + 8 > uw) uw = tlen(srp, USB_NOPEEK) + 8;
    if (hw > iw) iw = hw;
    if (sw > iw) iw = sw;
    if (ow > iw) iw = ow;
    if (uw > iw) iw = uw;
    /* the tester: per port a d-pad, LB RB over Start, and the four buttons */
    int bs = tlen(srp, "LB") + 14, bst = tlen(srp, "Start") + 12, bh = fh + 5;
    int sx = bs + 2, sy = bh + 2, pw = tlen(srp, "Joystick port") + 14;
    int mid = 2 * bs + 4 > bst ? 2 * bs + 4 : bst;
    int tw = pw + 3 * sx + 14 + mid + 14 + 3 * sx;
    if (tw > iw) iw = tw;
    int bw = 0;
    for (int i = 0; buttons[i]; i++) if (tlen(srp, buttons[i]) + 24 > bw) bw = tlen(srp, buttons[i]) + 24;

    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = w->vi;
    ng.ng_Height = gh;
    struct Gadget *g = CreateContext(&w->glist);
    ng.ng_LeftEdge = left + lw; ng.ng_TopEdge = y; ng.ng_Width = cw;
    ng.ng_GadgetText = (UBYTE *)"Controllers from"; ng.ng_GadgetID = G_SRC; ng.ng_Flags = PLACETEXT_LEFT;
    g = w->g_src = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)src_names, GTCY_Active, src_index(w), TAG_DONE);
    y += gh + 4;
    ng.ng_LeftEdge = left + lw; ng.ng_TopEdge = y; ng.ng_Width = cw;
    ng.ng_GadgetText = (UBYTE *)"Settings for"; ng.ng_GadgetID = G_FOR; ng.ng_Flags = PLACETEXT_LEFT;
    g = w->g_for = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)w->for_names, GTCY_Active, w->sel,
                                GA_Disabled, (!w->a314 && !w->usb) || !w->ngame, TAG_DONE);
    ng.ng_LeftEdge = left + lw + cw + 8; ng.ng_Width = 26;
    ng.ng_GadgetText = (UBYTE *)DPAD_LABEL; ng.ng_GadgetID = G_DPAD; ng.ng_Flags = PLACETEXT_RIGHT;
    g = w->g_dpad = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Checked, w->dpad, GTCB_Scaled, TRUE,
                                 GA_Disabled, !w->a314 && !w->usb, TAG_DONE);
    y += gh + 4;
    ng.ng_LeftEdge = left + lw; ng.ng_TopEdge = y; ng.ng_Width = cw; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetText = (UBYTE *)"Jump button"; ng.ng_GadgetID = G_JUMP;
    g = w->g_jump = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)jump_names, GTCY_Active, SET_JUMP(w->jump[w->sel]),
                                 GA_Disabled, !w->a314 && !w->usb, TAG_DONE);
    ng.ng_LeftEdge = left + lw + cw + 8; ng.ng_Width = ucw;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_MODE; ng.ng_Flags = 0;
    g = w->g_mode = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)mode_names, GTCY_Active, SET_MODE(w->jump[w->sel]),
                                 GA_Disabled, !w->a314 && !w->usb, TAG_DONE);
    y += gh + 4;
    ng.ng_LeftEdge = left + lw; ng.ng_TopEdge = y; ng.ng_Width = cw; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_GadgetText = (UBYTE *)"Game jumps with"; ng.ng_GadgetID = G_AS;
    g = w->g_as = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)as_names, GTCY_Active, SET_AS(w->jump[w->sel]),
                               GA_Disabled, !w->a314 && !w->usb, TAG_DONE);
    /* on the same row, so the window still fits a 640x256 Workbench. Non-negotiable */
    ng.ng_LeftEdge = left + lw + cw + 8; ng.ng_Width = ucw;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_UP; ng.ng_Flags = 0;
    g = w->g_up = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)up_names, GTCY_Active, up_mode(w->jump[w->sel]),
                               GA_Disabled, (!w->a314 && !w->usb) || !SET_AS(w->jump[w->sel]), TAG_DONE);
    y += gh + 2;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = y; ng.ng_Width = iw;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_HINT; ng.ng_Flags = 0;
    g = w->g_hint = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"", TAG_DONE);
    y += gh + 4;
    /* the tester's places: the joystick port first, since that's where a pad plays */
    w->port_x = left;
    for (int k = 0; k < 2; k++) {
        int port = 1 - k, ty = y + k * (3 * sy + 4);
        int x0 = left + pw, x1 = x0 + 3 * sx + 14, x2 = x1 + mid + 14;
        struct pbox *b = w->box[port];
        w->port_y[port] = ty + sy + (bh - fh) / 2;
        b[0] = (struct pbox){ x0 + sx, ty, bs, bh };                       /* up, the one everyone presses */
        b[1] = (struct pbox){ x0 + sx, ty + 2 * sy, bs, bh };              /* down, for the rare game that uses it */
        b[2] = (struct pbox){ x0, ty + sy, bs, bh };                       /* left, still a direction */
        b[3] = (struct pbox){ x0 + 2 * sx, ty + sy, bs, bh };              /* right, same deal */
        b[4] = (struct pbox){ x2 + sx, ty + 2 * sy, bs, bh };              /* A, the bottom one, where your thumb lands */
        b[5] = (struct pbox){ x2 + 2 * sx, ty + sy, bs, bh };              /* B, right of it */
        b[6] = (struct pbox){ x2, ty + sy, bs, bh };                       /* X, on the left */
        b[7] = (struct pbox){ x2 + sx, ty, bs, bh };                       /* Y, top of the diamond */
        b[8] = (struct pbox){ x1 + (mid - 2 * bs - 4) / 2, ty, bs, bh };   /* LB, the left shoulder */
        b[9] = (struct pbox){ x1 + (mid + 4) / 2, ty, bs, bh };            /* RB, the right shoulder */
        b[10] = (struct pbox){ x1 + (mid - bst) / 2, ty + sy, bst, bh };   /* Start, in the middle, as ever */
    }
    y += 2 * (3 * sy + 4) + 2;
    ng.ng_LeftEdge = left; ng.ng_TopEdge = y; ng.ng_Width = iw;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_STATE;
    g = w->g_state = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"", TAG_DONE);
    int yb = y + gh + 8;
    int bx[3] = { left, left + (iw - bw) / 2, left + iw - bw };
    for (int i = 0; i < 3; i++) {
        ng.ng_LeftEdge = bx[i]; ng.ng_TopEdge = yb; ng.ng_Width = bw;
        ng.ng_GadgetText = (UBYTE *)buttons[i]; ng.ng_GadgetID = G_SAVE + i; ng.ng_Flags = PLACETEXT_IN;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_DONE);
    }
    if (!g) goto fail;
    int ww = iw + 20, ih = yb + gh + 8 - title;
    w->win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"AGA-PISTORM - Controller",
        WA_Gadgets, (ULONG)w->glist,
        WA_InnerWidth, ww, WA_InnerHeight, ih,
        WA_Left, scr->Width > ww ? (scr->Width - ww) / 2 : 0,
        WA_Top, scr->Height > ih + title ? (scr->Height - ih - title) / 2 : 0,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_AutoAdjust, TRUE,
        WA_IDCMP, CHECKBOXIDCMP | CYCLEIDCMP | BUTTONIDCMP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
        TAG_DONE);
    if (!w->win) goto fail;
    GT_RefreshWindow(w->win, NULL);
    front_set(w->fr, w->win);
    if (w->usb) w->peek = (UBYTE)usbm_tester(1);
    set_hint(w);
    draw_tester(w);
    return w;
fail:
    padw_close(w);
    return NULL;
}

/* 0 open; 1 Cancel, 2 Use, 3 Save - stored then, not before */
static int padw_handle(struct padw *w)
{
    struct IntuiMessage *im;
    int rc = 0;
    while (!rc && (im = GT_GetIMsg(w->win->UserPort))) {
        ULONG cls = im->Class;
        UWORD code = im->Code;
        struct Gadget *gad = (struct Gadget *)im->IAddress;
        GT_ReplyIMsg(im);
        if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(w->win); draw_tester(w); GT_EndRefresh(w->win, TRUE); continue; }
        if (cls == IDCMP_CLOSEWINDOW || (cls == IDCMP_VANILLAKEY && code == 27)) { rc = 1; continue; }
        if (cls != IDCMP_GADGETUP) continue;
        int id = gad->GadgetID;
        if (id == G_SRC) {                        /* the tester catches up at the next tick */
            w->a314 = (code & 1) != 0;
            w->usb = (code & 2) != 0;
            if (w->usb != w->peek) w->peek = (UBYTE)usbm_tester(w->usb);
            set_used(w);
        } else if (id == G_FOR) {
            w->sel = code <= w->ngame ? code : 0;
            show_set(w);
        } else if (id == G_DPAD) {                 /* a game without its own follows D-PAD Jump, or gets nothing */
            w->dpad = (gad->Flags & GFLG_SELECTED) != 0;
            w->own_note = 0;
            for (int i = 1; i <= w->ngame; i++)
                if (!w->own[i] && !w->touched[i]) w->jump[i] = w->dpad ? w->jump[0] : 0;
            show_set(w);
        } else if (id == G_JUMP || id == G_MODE || id == G_AS || id == G_UP) {
            int s = w->jump[w->sel];
            if (id == G_JUMP) s = (s & ~SET_JUMP_M) | (code & 7);
            else if (id == G_MODE) s = (s & ~SET_MODE_M) | (code & 3) << 9;
            else if (id == G_AS) s = (s & ~SET_AS_M) | (code & 7) << 4;
            else s = with_up(s, code);
            w->jump[w->sel] = s = set_norm(s);
            if (w->sel) w->touched[w->sel] = 1;
            else w->own_note = 0;
            if (!w->sel && w->dpad)                      /* a game without its own follows the change */
                for (int i = 1; i <= w->ngame; i++) if (!w->own[i] && !w->touched[i]) w->jump[i] = s;
            show_set(w);
        }
        else if (id == G_SAVE) rc = 3;
        else if (id == G_USE) rc = 2;
        else if (id == G_CANCEL) rc = 1;
    }
    if (rc >= 2) {
        int save = rc == 3;
        store_all(w->jump[0], w->dpad, w->a314, w->usb, save);
        /* each game: its own if I set it here, or if it already had one */
        for (int i = 1; i <= w->ngame; i++)
            if (w->touched[i] || w->own[i]) pad_store(w->game[i], w->jump[i], save);
        pad_send_a314(w->a314);
        pad_send(pad_jump_for(NULL));
        usbm_entry(w->usb);
    }
    return rc;
}

int pad_window(void)
{
    int opened_intui = 0, opened_gfx = 0, opened_gt = 0, timer_open = 0, rc = 1;
    struct Screen *scr = NULL;
    struct padw *w = NULL;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39))) opened_intui = 1;
    if (!GfxBase && (GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39))) opened_gfx = 1;
    if (!GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39))) opened_gt = 1;
    if ((tport = CreateMsgPort()) && (treq = (struct timerequest *)CreateIORequest(tport, sizeof *treq)) &&
        !OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0))
        timer_open = 1;
    if (timer_open && IntuitionBase && GfxBase && GadToolsBase && (scr = LockPubScreen(NULL)) && (w = padw_open(scr))) {
        ULONG wsig = 1UL << w->win->UserPort->mp_SigBit, tsig = 1UL << tport->mp_SigBit;
        rc = 0;
        tick(w);
        show_pads(w);
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs = 0;
        treq->tr_time.tv_micro = 20000;
        SendIO((struct IORequest *)treq);
        while (!rc) {
            ULONG got = Wait(wsig | tsig | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C) rc = 1;
            if ((got & tsig) && CheckIO((struct IORequest *)treq)) {
                WaitIO((struct IORequest *)treq);
                tick(w);
                show_pads(w);
                treq->tr_time.tv_secs = 0;
                treq->tr_time.tv_micro = 20000;
                SendIO((struct IORequest *)treq);
            }
            if (!rc && (got & wsig)) rc = padw_handle(w);
        }
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
    }
    if (w) padw_close(w);
    if (scr) UnlockPubScreen(NULL, scr);
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (opened_gt)    { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (opened_gfx)   { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (opened_intui) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    return rc - 1;
}

/* ---- agaboot PAD, the command line ------------------------------------------ */
int pad_command(int argc, char **argv)
{
    const char *game = NULL;
    char gname[32];
    int jump = -1, as = -1, upm = -1, mode = -1, dpad = -1, a314 = -1, usb = -1, save = 0, remove = 0, ask = 0;
    for (int i = 2; i < argc; i++) {
        if (!stricmp(argv[i], "ASK")) ask = 1;
        else if (!stricmp(argv[i], "SAVE")) save = 1;
        else if (!stricmp(argv[i], "REMOVE")) remove = 1;
        else if (!strnicmp(argv[i], "GAME=", 5) && argv[i][5]) {
            /* GAME="x": the shell leaves the quotes in, the drawer has none. Strip the
   buggers. */
            const char *g = argv[i] + 5;
            int n = strlen(g);
            if (n >= 2 && g[0] == '"' && g[n - 1] == '"') { g++; n -= 2; }
            if (n > (int)sizeof gname - 1) n = sizeof gname - 1;
            memcpy(gname, g, n);
            gname[n] = 0;
            game = gname[0] ? gname : NULL;
        }
        else if (!strnicmp(argv[i], "JUMP=", 5) && (jump = pad_parse_jump(argv[i] + 5)) >= 0) ;
        else if (!strnicmp(argv[i], "AS=", 3) && (as = pad_parse_jump(argv[i] + 3)) >= 0) ;
        else if (!stricmp(argv[i], "UP=ALSO")) upm = 0;
        else if (!stricmp(argv[i], "UP=ONLY")) upm = 1;
        else if (!stricmp(argv[i], "UP=PLAIN")) upm = 2;
        else if (!strnicmp(argv[i], "MODE=", 5) && (mode = parse_mode(argv[i] + 5)) >= 0) ;
        else if (!stricmp(argv[i], "DPAD=ON")) dpad = 1;
        else if (!stricmp(argv[i], "DPAD=OFF")) dpad = 0;
        else if (!stricmp(argv[i], "A314=ON")) a314 = 1;
        else if (!stricmp(argv[i], "A314=OFF")) a314 = 0;
        else if (!stricmp(argv[i], "USB=ON")) usb = 1;
        else if (!stricmp(argv[i], "USB=OFF")) usb = 0;
        else {
            PutStr("agaboot: PAD [GAME=<drawer>] [JUMP=UP|A|B|X|Y|LB|RB|START] [AS=UP|A|B|X|Y|LB|RB|START]\n"
                   "            [UP=ALSO|ONLY|PLAIN] [MODE=CD32|JOY2|JOY1] [DPAD=ON|OFF] [A314=ON|OFF]\n"
                   "            [USB=ON|OFF] [SAVE] | REMOVE | ASK\n");
            return 5;
        }
    }
    if (ask) { pad_window(); return 0; }
    if ((a314 >= 0 || usb >= 0 || dpad >= 0) && game) { PutStr("agaboot: PAD A314=, USB= and DPAD= are for all games\n"); return 5; }
    if (remove) {
        if (!game) { PutStr("agaboot: PAD GAME=<drawer> REMOVE\n"); return 5; }
        pad_forget(game);
        Printf("agaboot: %s jumps as all games again\n", (ULONG)game);
        return 0;
    }
    if (jump >= 0 || as >= 0 || upm >= 0 || mode >= 0) {   /* what isn't named stays as it was - don't reset the lot */
        int s = game ? pad_jump_for(game) : pad_preset();
        if (jump >= 0) s = (s & ~SET_JUMP_M) | jump;
        if (as >= 0) s = (s & ~SET_AS_M) | as << 4;
        if (upm >= 0) s = with_up(s, upm);
        if (mode >= 0) s = (s & ~SET_MODE_M) | mode << 9;
        s = set_norm(s);
        pad_store(game, s, save);
    }
    if (dpad >= 0) pad_store_dpad(dpad, save);
    if (!game && (jump >= 0 || as >= 0 || upm >= 0 || mode >= 0 || dpad >= 0)) pad_send(pad_jump_for(NULL));
    if (a314 >= 0) {
        pad_store_a314(a314, save);
        pad_send_a314(a314);
    }
    if (usb >= 0) {
        pad_store_usb(usb, save);
        usbm_entry(usb);
    }
    /* what is set, and what the kernel actually says */
    Printf("The A314's pads and mouse in games: %s\n", (ULONG)(pad_use_a314() ? "used" : "not used"));
    Printf("The PiStorm's USB pad and mouse in games: %s\n", (ULONG)(pad_use_usb() ? "used" : "not used"));
    Printf(DPAD_NAME ": %s\n", (ULONG)(pad_dpad_on() ? "on in all games" : "off (games get Up as a joystick's)"));
    Printf(DPAD_NAME ": jump button %s; the game jumps with %s\n", (ULONG)jump_names[SET_JUMP(pad_preset())],
           (ULONG)as_text(pad_preset()));
    Printf(DPAD_NAME ": a pad is a %s to the game\n", (ULONG)mode_names[SET_MODE(pad_preset())]);
    if (game) {
        const char *own = pad_has_own(game) ? " (its own)" : " (as all games)";
        Printf("Jump button, %s: %s%s\n", (ULONG)game, (ULONG)jump_names[SET_JUMP(pad_jump_for(game))], (ULONG)own);
        Printf("Game jumps with, %s: %s%s\n", (ULONG)game, (ULONG)as_text(pad_jump_for(game)), (ULONG)own);
        Printf("A pad, %s: a %s%s\n", (ULONG)game, (ULONG)mode_names[SET_MODE(pad_jump_for(game))], (ULONG)own);
    }
    if (!our_kernel()) { PutStr("The AGA-PISTORM kernel is not running here.\n"); return 0; }
    struct pad_state p;
    ULONG b0, b1, word[2];
    kernel_pad(&p);
    if (!p.a314 != !pad_use_a314())                /* it gets told at the next game, not now */
        Printf("The kernel has them %s until the next game.\n", (ULONG)(p.a314 ? "used" : "not used"));
    /* what the last game (or Use) sent; an older kernel just says Up */
    Printf("The kernel has: jump button %s; the game jumps with %s; a pad is a %s.\n", (ULONG)jump_names[p.jump & 7],
           (ULONG)as_text(set_norm((int)((p.as & 7) << 4 | ((p.as & 8) ? SET_ONLY : 0) | ((p.as & 16) ? SET_PLAIN : 0)))),
           (ULONG)mode_names[p.mode <= 2 ? p.mode : 0]);
    if (!p.addr) { PutStr("No pads: A314Pad is not running on this Amiga.\n"); return 0; }
    Printf("The A314's pad block is at $%08lx", p.addr);
    if (p.ca) Printf(", a314.device's ComArea at $%08lx", p.ca);
    PutStr(".\n");
    if (!read_block(p.addr, &b0, word)) { PutStr("The A314's Pi does not write the pads (a314pad on the Pi?).\n"); return 0; }
    Delay(60);                                     /* the Pi writes 4 times a second at least - give it a chance */
    if (!read_block(p.addr, &b1, word) || b1 == b0) { PutStr("The A314's Pi does not write the pads (a314pad on the Pi?).\n"); return 0; }
    Printf("The A314's pads are live: joystick port $%08lx, mouse port $%08lx.\n", word[1], word[0]);
    return 0;
}
