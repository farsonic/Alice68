/* AGA-PISTORM - the A314's gamepads, doing a game's joystick ports.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A Pi on an A314 board reads the gamepads plugged into it (USB) or paired
 * with it (Bluetooth), and its a314pad service turns each one into the word
 * lowlevel.library's ReadJoyPort answers for a CD32 pad. On the Workbench the
 * Amiga's A314Pad hands those words to the programs that ask the OS. A WHDLoad
 * game asks nobody: the bastard reads the port's chips itself - JOYxDAT, the
 * CIA's fire bits, POTGOR, and a CD32 pad's shift register clocked through the
 * fire line - and the OS A314Pad runs on is gone.
 *
 * In the sandbox those reads are mine, damn it. So A314Pad sets a small block
 * aside in the A314's memory and gives me its 68k address ($DFF1EC/$DFF1EE,
 * then $DFF1F2 = $5B41; $5B40 forgets it), and the A314's Pi writes the block
 * over the A314 itself (a314d's MSG_WRITE_MEM) - no Amiga program in the way,
 * so it carries on under WHDLoad:
 *
 *   +0   'A3PD'          the magic
 *   +4   beat            +1 at every write: each change, and 4 times a second
 *   +8   port 0's word   the mouse port; 0 = no pad plays there
 *   +12  port 1's word   the joystick port
 *   +16  ports 2, 3      (the parallel port's adapters - not read here)
 *
 * all big-endian, the words as ReadJoyPort's: JP_TYPE_GAMECTLR (a CD32 pad) or
 * JP_TYPE_JOYSTK in the top nibble, JPF_* buttons and directions below. No
 * exceptions and no clever byte swapping, I checked.
 *
 * The housekeeper core reads the block over the bus while the sandbox is up,
 * one long a visit and a round every 4 ms. It is live while the beat moves: a
 * second without, and the ports are the real ones again. Once WHDLoad has the
 * machine (game_taken), core 0 adds the pads to what the game reads -
 * directions into JOYxDAT, red into CIA-A's fire bit, blue into POTGOR's pin
 * 9, and a CD32 pad's shift register that follows the game's POTGO and CIA-A
 * writes, the way WinUAE's inputdevice.cpp models the pad: pin 5 held low by
 * POTGO selects it, each falling edge of the fire line (an output then)
 * shifts, and pin 9 reads blue, red, yellow, green, forward, reverse, play,
 * then a 1 and 0s. A joystick in the port keeps working: its directions and
 * buttons stay, the pad's are added on top. I didn't invent that; I copied it
 * and checked it against the emulator like a reasonable man.
 *
 * A mouse word (JP_TYPE_MOUSE: JOYxDAT's two counters, left/right/middle on
 * red/blue/play) is added the same way: its counters to the port's real ones,
 * so a real mouse there still moves too, counted from where they stood when
 * the game took the machine; left on the fire bit, right on pin 9, middle on
 * pin 5.
 *
 * One pad button can be the jump (agaboot's Controller window, $5B50 + n):
 * most Amiga games jump with the stick pushed up, so that button gives the
 * game Up instead of itself, and Up on the pad still jumps too.
 *
 * Some games jump with a button instead (Turrican 2 AGA in its CD32 mode
 * jumps on red and ignores Up): $5B60 + t names the button the game jumps
 * with, and the jump button and Up on the pad both press it. Up stays Up as
 * well, for the games that climb with it; $5B70 + t takes Up away instead,
 * for the games that would do something else with it; $5B80 + t leaves Up
 * alone and only the jump button presses it - for the games whose menus pick
 * with that button, where Up pressing it too would pick instead of move.
 * Three ways to hang yourself, one setting.
 *
 * A pad is a CD32 pad to the game (its shift register on pin 9 and the fire
 * line as the clock), as on a real A1200 with one: games that look for it
 * switch to their CD32 controls. $5B90 + m makes it a plain joystick instead,
 * m 1 with two buttons (red on fire, blue on pin 9) and m 2 with one (red
 * only, pin 9 left alone): no shift register to find, so such a game keeps
 * its joystick controls, and a game upset by a second button gets none.
 *
 * A314Pad can also name a314.device's ComArea ($5B45, 520 bytes): its rings
 * to and from the Pi, which a314d goes on filling while the game runs. The
 * leave keeps it as the Pi left it, like the block - put back as it was at
 * entry, the ring and its tail would no longer be the ones a314d keeps a copy
 * of, and the Amiga would read the rolled-back bytes as packets (the A314
 * agent, 30.10.2026). Only the Pi writes it while WHDLoad has the machine;
 * while the OS runs in the sandbox, a314.device reads and writes the real one
 * (aga_ca_lo/hi below). I learned that one the hard way.
 *
 * The Pi's own USB mouse (aga_usb.c, while a game has the machine) comes in
 * as a mouse word of its own, for port 0: alone, or added to the A314's
 * mouse there. An A314 pad playing in port 0 wins over it. The Pi's own USB
 * pad comes in as a pad word for port 1: alone, or with an A314 pad there,
 * both pads' directions and buttons together; the jump button and the pad
 * modes apply to it as to the A314's.
 */
#include <stdint.h>
#include "aga/chipset.h"
#include "aga/regs.h"
#include "aga_glue.h"

extern void kprintf(const char *fmt, ...);
#include "ps_protocol.h"    /* the bus API differs per board; don't redeclare it or it bites */

#define PAD_MAGIC      0x41335044u                 /* 'A3PD' - don't touch, it's the handshake */
#define JPF_RIGHT      0x1u
#define JPF_LEFT       0x2u
#define JPF_DOWN       0x4u
#define JPF_UP         0x8u
#define JPF_RED        (1u << 22)                  /* a mouse's left button, god help me */
#define JPF_BLUE       (1u << 23)                  /* ...its right one, same deal */
#define JPF_PLAY       (1u << 17)                  /* ...its middle one, the wheel click */
#define TYPE_GAMECTLR  1u                          /* JP_TYPE_GAMECTLR >> 28, if you must know */
#define TYPE_MOUSE     2u                          /* counters in bits 0-7 (across) and 8-15 (down), damn it */
#define TYPE_JOYSTK    3u
#define CA_BYTES       520u                        /* the ComArea: 516 bytes on the TD, 520 on the FE, naturally */

/* set by the housekeeper, read by core 0 on every damn input read */
volatile int aga_pad_on;
static volatile uint32_t pad_addr;                 /* the block's 68k address; 0 means none, and I mean none */
static volatile uint32_t ca_addr;                  /* a314.device's ComArea; 0 means none */
static volatile uint32_t pad_word[2];              /* ports 0 and 1 as I last read the bastards */
static volatile uint32_t jump_bit;                 /* the JPF button that jumps; 0 = none */
static int jump_n;
static volatile uint32_t jump_to;                  /* the JPF button the game jumps with; 0 = Up; | TO_ONLY */
static int jump_as;                                /* ...as $5B60 + t named the bastard: t, | 8 TO_ONLY, | 16 TO_PLAIN */
#define TO_ONLY        0x80000000u                 /* Up on the pad is no longer Up (outside the JPF buttons) */
#define TO_PLAIN       0x40000000u                 /* Up on the pad stays just Up: only the jump button presses it */
static volatile uint32_t usb_word;                 /* the Pi's own USB mouse for port 0 (aga_usb.c); 0 = none */
static volatile uint32_t usb_pad;                  /* ...its USB pad for port 1; 0 = none */
static volatile int use_a314;                      /* agaboot's Controllers from: off until the bugger says otherwise */
static volatile int pad_mode;                      /* 0 a CD32 pad, 1 a 2-button joystick, 2 a 1-button one; no other crap */

static inline uint64_t cnt_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }
static inline uint64_t cnt_frq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f; }

/* "[AGA] pad: <what> $xxxxxxxx" into the diag log - lock-free, any core */
static void pad_log(const char *what, uint32_t v)
{
    static char line[96];
    static const char hex[] = "0123456789ABCDEF";
    const char *head = "[AGA] pad: ";
    int n = 0;
    while (*head && n < 40) line[n++] = *head++;
    while (*what && n < 84) line[n++] = *what++;
    line[n++] = ' '; line[n++] = '$';
    for (int s = 28; s >= 0; s -= 4) line[n++] = hex[(v >> s) & 0xF];
    line[n] = 0;
    aga_diag_log(line);
}

/* ---- the block: the 68k registers it, the housekeeper reads it, I swear ---- */

/* $DFF1EC / $DFF1EE as the 68k last wrote them, in every mode. A314Pad
   registers from the ECS desktop, where the emulated chipset is off and those
   writes go on to the real chip - so vectors.c notes the bastards here on the
   way past. */
static volatile uint16_t addr_word[2];
void aga_pad_addr_word(int low, uint16_t v) { addr_word[low ? 1 : 0] = v; }
uint32_t aga_pad_addr_written(void) { return ((uint32_t)addr_word[0] << 16) | addr_word[1]; }

/* the housekeeper core (aga_usb.c): the USB mouse's and pad's words; 0 = none */
void aga_pad_usb_mouse(uint32_t w) { __atomic_store_n(&usb_word, w, __ATOMIC_RELAXED); }
void aga_pad_usb_pad(uint32_t w) { __atomic_store_n(&usb_pad, w, __ATOMIC_RELAXED); }

/* The block's and the ComArea's ranges, [lo, hi) or empty, for the sandbox's
   leave: it copies the real chip RAM back from the emulated one, and the
   A314's memory is real chip RAM on a 2 MB A500 (the A314-500's, $100000
   up). Put back as it was at entry, the block would be a stranger to the Pi,
   which reads it back before each write - it would stop for good after the
   first game. That's what I'm avoiding, in case it wasn't obvious. */
void aga_pad_kept(uint32_t lo[2], uint32_t hi[2])
{
    uint32_t a = __atomic_load_n(&pad_addr, __ATOMIC_ACQUIRE), c = __atomic_load_n(&ca_addr, __ATOMIC_ACQUIRE);
    lo[0] = a; hi[0] = a ? a + 24 : 0;
    lo[1] = c; hi[1] = c ? c + CA_BYTES : 0;
}

/* ---- a314.device's ComArea while the OS runs in the sandbox --------------
 *
 * The sandbox serves all of chip RAM from the emulated copy, and so it did
 * the ComArea while the OS ran in there: from the entry until WHDLoad took
 * the machine, and from the game's quit until the leave. a314.device read a
 * frozen ring there while the A314's Pi filled the real one, took the A314's
 * doorbell (A_EVENTS, cleared by its read on the real clock port) for
 * nothing and went back to sleep. The Pi stopped once the real ring was
 * full, waiting for the Amiga, which waited for the Pi: after every WHDLoad
 * game the A314's mouse was dead on the Workbench, r2a holding 252 bytes and
 * a2r empty (the user and the A314 agent, 30.10.2026). What a314.device put
 * in a2r in there went to the copy and was lost. A textbook deadlock, and I
 * had to find it in the middle of a pile of Amiga weirdness.
 *
 * So while the OS runs, the 68k's accesses to the ComArea go to the real bus
 * (aga_chip_read/write ask aga_ca_lo/hi). While the game has the machine
 * they stay in the copy: a game with a big BaseMem owns that memory, and the
 * leave keeps the real ComArea anyway.
 *
 * The game's turn starts at the INTENA clear with EXTER that sets game_taken
 * (aga_pad_taken). The OS's starts with a314.device itself: before every
 * look at its rings it switches the A314's clock port to the CMEM, register
 * D read and written back with bit 3 set ($DC0037; int_server_td.asm,
 * cmem.c, driver_task.c). WHDLoad has put the OS's memory and interrupts
 * back by then, at the quit and at each OS switch of a game that loads from
 * disk. A Kickstart a KickEmu game boots may write register D too, so only
 * that read-then-or-8 pair counts. Anything else and I'd be chasing ghosts.
 *
 * The old MMU path (aga.mmu) maps the copy and has none of this. */
volatile uint32_t aga_ca_lo, aga_ca_hi;            /* [lo, hi) from the real bus; hi 0 = none */
static int ca_os;                                  /* the OS runs in the sandbox, not a game */
static int regd_read = -1;                         /* register D as just read, or -1 */
static unsigned ca_returns;                        /* the OS's returns this session, for the leave's log */

static void ca_route(void)
{
    uint32_t c = __atomic_load_n(&ca_addr, __ATOMIC_ACQUIRE);
    int on = ca_os && aga_sandbox_active && c;
    aga_ca_hi = 0;                                 /* shut while the edges change - no half-open window, ever */
    aga_ca_lo = on ? c : 0;
    aga_ca_hi = on ? c + CA_BYTES : 0;
}

/* aga_glue.c, core 0: the sandbox went in (the OS runs in it) or came out */
void aga_pad_sandbox(int entered)
{
    if (!entered && ca_addr) pad_log("a314.device's ComArea, the OS's returns to it this session:", ca_returns);
    ca_os = entered;
    ca_returns = 0;
    regd_read = -1;
    ca_route();
}

/* aga_glue.c, core 0: WHDLoad (or some program under Run) grabbed the machine */
void aga_pad_taken(void)
{
    if (!ca_os) return;
    ca_os = 0;
    ca_route();
}

/* vectors.c, core 0: the 68k read or wrote the clock port's register D in
   the sandbox. I only note the bugger - the access goes on to the real clock
   port. */
void aga_pad_regd(uint8_t v, int write)
{
    int r = regd_read;
    if (!write) { regd_read = v & 0xF; return; }
    regd_read = -1;
    if (r < 0 || (v & 0xF) != (r | 8) || ca_os) return;
    ca_os = 1;
    ca_returns++;
    ca_route();
}

/* aga_chip_read/write, for an access that touches [aga_ca_lo, aga_ca_hi).
   All of it inside: 0, and the real bus does the whole damn thing. Across an
   edge: the ComArea's bytes from the bus, the rest from the copy (chip). */
int aga_pad_ca_read(const uint8_t *chip, uint64_t far, int size, uint64_t *value, uint64_t *value2)
{
    uint32_t lo = aga_ca_lo, hi = aga_ca_hi;
    if (far >= lo && far + (uint64_t)size <= hi) return 0;
    uint64_t v[2] = { 0, 0 };
    for (int i = 0; i < size; i++) {
        uint32_t a = (uint32_t)far + (uint32_t)i;
        uint8_t b = (a >= lo && a < hi) ? (uint8_t)ps_read_8(a) : chip[a];
        v[i >> 3] = v[i >> 3] << 8 | b;
    }
    *value = v[0];
    if (size == 16) *value2 = v[1];
    return 1;
}

int aga_pad_ca_write(uint8_t *chip, uint64_t far, int size, uint64_t value, uint64_t value2)
{
    uint32_t lo = aga_ca_lo, hi = aga_ca_hi;
    if (far >= lo && far + (uint64_t)size <= hi) return 0;
    int last = (size < 8 ? size : 8) - 1;          /* each 64-bit half big-endian, of course */
    for (int i = 0; i < size; i++) {
        uint32_t a = (uint32_t)far + (uint32_t)i;
        uint8_t b = (uint8_t)((i < 8 ? value : value2) >> (8 * (last - (i & 7))));
        if (a >= lo && a < hi) ps_write_8(a, b);
        else chip[a] = b;
    }
    return 1;
}

/* the A314's memory is the trapdoor RAM: chip RAM on a 1 MB Agnus, else slow
   RAM. Anything else is not the A314's, and reading it is not my job. */
static int a314_memory(uint32_t a, uint32_t n)
{
    return !(a & 3) && (a + n <= 0x200000 || (a >= 0xC00000 && a + n <= 0xD80000));
}

static void comarea_register(uint32_t addr)
{
    if (addr && !a314_memory(addr, CA_BYTES)) {
        kprintf("[AGA] pad: ComArea at $%x refused (not in chip or slow RAM)\n", addr);
        pad_log("ComArea refused (not in chip or slow RAM) at", addr);
        return;
    }
    uint32_t old = __atomic_exchange_n(&ca_addr, addr, __ATOMIC_ACQ_REL);
    ca_route();
    if (addr || old)
        pad_log(addr ? "the A314's ComArea kept at the leave, at" : "the A314's ComArea forgotten, it was at", addr ? addr : old);
}

/* core 0, $DFF1F2 = $5B40 + cmd: 1 the block, 5 the ComArea, at the address
   just written; 4 forgets the ComArea, 0 both. Clear enough, I hope. */
int aga_pad_command(int cmd)
{
    uint32_t a = ((uint32_t)addr_word[0] << 16) | addr_word[1];
    if (cmd & 4) { comarea_register((cmd & 1) ? a : 0); return 1; }
    aga_pad_register((cmd & 1) ? a : 0);
    if (!(cmd & 1)) comarea_register(0);
    return 1;
}

/* core 0, $5B50 + n: the button that jumps - 0 none, then A/red, B/blue,
   X/yellow, Y/green, LB/reverse, RB/forward, Start/play (the F710's names as
   a314pad maps the ugly thing). agaboot sends it at every game's entry. */
void aga_pad_jump(int n)
{
    static const uint8_t bit[8] = { 0, 22, 23, 21, 20, 18, 19, 17 };
    n &= 7;
    if (n != jump_n) pad_log("the jump button is now", (uint32_t)n);
    jump_n = n;
    __atomic_store_n(&jump_bit, n ? 1u << bit[n] : 0u, __ATOMIC_RELAXED);
}

/* core 0, $5B60 + t: the button the game jumps with - 0 Up, as a joystick,
   then A to Start as above - and + $10 when Up on the pad presses only that
   button, no longer Up (v bit 3 here); $5B80 + t when Up stays just Up and
   only the jump button presses it (v bit 4). agaboot sends it with the jump
   button, because of course it does. */
void aga_pad_jump_as(int v)
{
    static const uint8_t bit[8] = { 0, 22, 23, 21, 20, 18, 19, 17 };
    int t = v & 7;
    v = t ? v & 31 : 0;                            /* only Up: there's nothing to take away or keep */
    if (v & 16) v &= ~8;                           /* Up just Up: nothing to take from the poor bastard */
    if (v != jump_as) pad_log("the game jumps with button (8: Up no longer Up, 16: Up just Up)", (uint32_t)v);
    jump_as = v;
    __atomic_store_n(&jump_to, t ? (1u << bit[t]) | ((v & 8) ? TO_ONLY : 0u) | ((v & 16) ? TO_PLAIN : 0u) : 0u,
                     __ATOMIC_RELAXED);
}

/* core 0, $5B90 + m: what a pad is to the game - 0 a CD32 pad, 1 a joystick
   with two buttons, 2 with one (m as $5B50's n). agaboot sends it with the
   jump button at every game's entry, same as the rest. */
void aga_pad_mode(int m)
{
    m = (m & 7) <= 2 ? m & 7 : 0;
    if (m != pad_mode) pad_log("a pad is now (0 a CD32 pad, 1 a 2-button joystick, 2 a 1-button one)", (uint32_t)m);
    __atomic_store_n(&pad_mode, m, __ATOMIC_RELAXED);
}

/* core 0, $5B49 / $5B48: the A314's pads and mouse used in games, or not.
   Only what the game gets changes: the block stays registered, the Pi goes on
   writing it, and the leave still keeps it and the ComArea (aga_pad_kept) -
   put back as they were at entry they would cut the A314's link, so
   Controllers from can never do that. The kernel starts with them not used,
   as agaboot's None. */
void aga_pad_use_a314(int on)
{
    on = !!on;
    if (on != use_a314) pad_log(on ? "the A314's pads used in games again" : "the A314's pads not used in games", 0);
    use_a314 = on;
}

/* agaboot's Controller window, through aga_diag.c's SEL_PAD: 0 the block's
   address, 1 the jump button, 2 the ComArea's address, 3 the A314 used, 4 the
   button the game jumps with (| 8: Up no longer Up, | 16: Up just Up), 5 what
   a pad is to the game, 6 the PiStorm USB pad's word (aga_usb.c; 0 while
   neither a game borrows it nor the window's tester reads the damn thing) */
uint32_t aga_pad_info(int i)
{
    return i == 0 ? __atomic_load_n(&pad_addr, __ATOMIC_ACQUIRE)
         : i == 1 ? (uint32_t)jump_n
         : i == 2 ? __atomic_load_n(&ca_addr, __ATOMIC_ACQUIRE)
         : i == 3 ? (uint32_t)use_a314
         : i == 4 ? (uint32_t)jump_as
         : i == 5 ? (uint32_t)pad_mode
         : i == 6 ? __atomic_load_n(&usb_pad, __ATOMIC_RELAXED) : 0;
}

/* core 0, $5B41 / $5B40 - register the block or forget it */
void aga_pad_register(uint32_t addr)
{
    if (addr && !a314_memory(addr, 24)) {
        kprintf("[AGA] pad: block at $%x refused (not in chip or slow RAM)\n", addr);
        pad_log("block refused (not in chip or slow RAM) at", addr);
        return;
    }
    uint32_t old = __atomic_exchange_n(&pad_addr, addr, __ATOMIC_ACQ_REL);
    if (addr) kprintf("[AGA] pad: the A314's pads in the block at $%x\n", addr);
    else      kprintf("[AGA] pad: the A314's pads forgotten\n");
    pad_log(addr ? "the A314's block registered at" : "the A314's block forgotten, it was at", addr ? addr : old);
}

/* The housekeeper core, every visit (aga_audio_bus_service): one long of the
   block at most, so the IPL sampling that loop exists for never waits behind
   more than two bus cycles of mine. Keep it that way. */
void aga_pad_service(void)
{
    static uint32_t polled, rd[4], beat;
    static uint64_t next, beat_at;
    static int step, beat_known, live, mouse_was[2];
    static uint16_t mouse_base[2];
    uint32_t a = __atomic_load_n(&pad_addr, __ATOMIC_ACQUIRE);
    int usb = aga_usb_live && aga_sandbox_active && aga_glue_game_taken();
    if (!a || !aga_sandbox_active || !use_a314) {
        if (live || polled) { __atomic_store_n(&pad_word[0], 0, __ATOMIC_RELAXED); __atomic_store_n(&pad_word[1], 0, __ATOMIC_RELAXED); }
        polled = 0; live = 0;
        mouse_was[0] = mouse_was[1] = 0;           /* back on: the mouse counts again from there */
        if (usb != aga_pad_on) {
            __atomic_store_n(&aga_pad_on, usb, __ATOMIC_RELEASE);
            pad_log(usb ? "in the game, the Pi's USB mouse (1) and pad (2)" : "off, the real ports in", usb ? aga_usb_live : 0);
        }
        return;
    }
    uint64_t now = cnt_now();
    if (a != polled) { polled = a; step = 0; beat_known = 0; live = 0; next = now; }
    if (step == 0 && now < next) return;
    rd[step] = ps_read_32(a + 4u * (uint32_t)step);
    if (++step < 4) return;
    step = 0;
    uint64_t f = cnt_frq();
    next = now + f / 250u;                         /* a full round every 4 ms - no dawdling */
    int was = live;
    if (rd[0] != PAD_MAGIC) { beat_known = 0; live = 0; }
    else if (!beat_known) { beat_known = 1; beat = rd[1]; }    /* trusted once the bugger has moved */
    else if (rd[1] != beat) { beat = rd[1]; beat_at = now; live = 1; }
    else if (now - beat_at > f) live = 0;          /* the Pi went quiet on me: a second without a beat */
    int on = (live && aga_glue_game_taken()) || usb;
    if (!live) { __atomic_store_n(&pad_word[0], 0, __ATOMIC_RELAXED); __atomic_store_n(&pad_word[1], 0, __ATOMIC_RELAXED); }
    if (live) {
        for (int i = 0; i < 2; i++) {
            uint32_t w = rd[2 + i];
            /* a mouse's counters from where they stood until the game took the
               machine: the game sees it move from there, never jump, and I
               spent a while getting that right. */
            if ((w >> 28) == TYPE_MOUSE) {
                if (!mouse_was[i] || !on) mouse_base[i] = (uint16_t)w;
                w = (w & ~0xFFFFu) | (((w >> 8) - (mouse_base[i] >> 8)) & 0xFFu) << 8 | ((w - mouse_base[i]) & 0xFFu);
            }
            mouse_was[i] = (rd[2 + i] >> 28) == TYPE_MOUSE;
            __atomic_store_n(&pad_word[i], w, __ATOMIC_RELAXED);
        }
    }
    if (live != was) pad_log(live ? "the A314's Pi writes the block, port 1" : "the A314's Pi went quiet, beat", live ? rd[3] : beat);
    if (on != aga_pad_on) {
        __atomic_store_n(&aga_pad_on, on, __ATOMIC_RELEASE);   /* after the words, always - ordering matters, damn it */
        pad_log(on ? "in the game, port 1" : "off, the real ports in", on ? rd[3] : 0);
    }
}

/* ---- what the game writes and reads (core 0, trap path, hold on tight) ----- */

static uint16_t potgo;                             /* POTGO as the game last wrote it in the sandbox */
static uint8_t  cia_pra = 0xFF, cia_ddra = 0x03;   /* CIA-A as last written (the OS's DDRA, for what it's worth) */
static uint8_t  clk_was[2];
static int      shifter[2] = { 8, 8 };

static inline uint32_t pad(int port)               /* a pad's or a mouse's word, or 0 if there's nothing there */
{
    uint32_t w = __atomic_load_n(&pad_word[port], __ATOMIC_RELAXED), t = w >> 28;
    uint32_t j = __atomic_load_n(&jump_bit, __ATOMIC_RELAXED), to = __atomic_load_n(&jump_to, __ATOMIC_RELAXED);
    int m = __atomic_load_n(&pad_mode, __ATOMIC_RELAXED);
    uint32_t u = port ? 0 : __atomic_load_n(&usb_word, __ATOMIC_RELAXED);
    uint32_t up = port ? __atomic_load_n(&usb_pad, __ATOMIC_RELAXED) : 0;
    if (up && (!w || t == TYPE_GAMECTLR || t == TYPE_JOYSTK)) {   /* the USB pad in port 1, merged with the A314's */
        w = (TYPE_GAMECTLR << 28) | ((w | up) & 0x0FFFFFFFu);
        t = TYPE_GAMECTLR;
    }
    if (u && !w) return u;                         /* the USB mouse alone in port 0 - rare, but it happens */
    if (u && t == TYPE_MOUSE)                      /* ...or with the A314's: counters added, both sets of buttons */
        return (TYPE_MOUSE << 28) | ((w | u) & 0x0FFF0000u) | ((((w >> 8) + (u >> 8)) & 0xFFu) << 8) | ((w + u) & 0xFFu);
    if (t == TYPE_MOUSE) return w;
    if (t != TYPE_GAMECTLR && t != TYPE_JOYSTK) return 0;
    if (!to) {
        if (w & j) w = (w & ~(j | JPF_DOWN)) | JPF_UP;   /* the jump button gives Up instead of itself - don't ask */
    } else {
        uint32_t up = w & JPF_UP;                  /* the game jumps with a button: the jump button presses that one */
        if ((w & j) || (up && !(to & TO_PLAIN)))   /* ...and Up too, unless Up stays just Up */
            w = (w & ~j) | (to & ~(TO_ONLY | TO_PLAIN));
        if (up && (to & TO_ONLY)) w &= ~JPF_UP;
    }
    if (m)                                         /* a joystick: no shift register; one button: no pin 9 either */
        w = (w & 0x0FFFFFFFu & ~(m == 2 ? JPF_BLUE : 0u)) | (TYPE_JOYSTK << 28);
    return w;
}
/* POTGO holds pin 5 low (OUTxX set, DATxX clear): the CD32 shift register */
static inline int cd32_mode(int port)
{
    return (potgo & (0x0200u << (4 * port))) && !(potgo & (0x0100u << (4 * port)));
}

void aga_pad_potgo(uint16_t v)
{
    potgo = v;
    for (int i = 0; i < 2; i++)
        if (!cd32_mode(i)) shifter[i] = 8;         /* pin 5 up: the register reloads, blue first, every damn time */
}

/* CIA-A's PRA ($BFE001) and DDRA ($BFE201) as written - a byte on the odd
   address, or the low half of a word on the even one. The write itself goes
   on to the real CIA as before. */
void aga_pad_cia_write(uint64_t far, int size, uint64_t value)
{
    uint8_t b;
    if (size == 1 && (far & 1)) b = (uint8_t)value;
    else if (size == 2 && !(far & 1)) b = (uint8_t)value;
    else return;
    if (far & 0x200) cia_ddra = b; else cia_pra = b;
    for (int i = 0; i < 2; i++) {
        uint8_t m = (uint8_t)(0x40 << i);
        /* the fire line as an output going low: the pad shifts, one bit closer */
        if (cd32_mode(i) && (cia_ddra & m) && clk_was[i] && !(cia_pra & m) && shifter[i] > 0)
            shifter[i]--;
        clk_was[i] = cia_ddra & cia_pra & m;
    }
}

/* JOY0DAT / JOY1DAT / POTGOR as the real chips answered - the genuine shit */
uint16_t aga_pad_custom(uint32_t reg, uint16_t v)
{
    if (reg == JOY0DAT || reg == JOY1DAT) {
        uint32_t w = pad(reg == JOY1DAT), d = w & 0xF;
        if ((w >> 28) == TYPE_MOUSE)               /* the counters added, each byte on its own, the way the hardware does it */
            return (uint16_t)((((v >> 8) + (w >> 8)) & 0xFFu) << 8 | ((v + w) & 0xFFu));
        if (!d) return v;
        /* right = bit 1, left = bit 9, down = bit 1 ^ bit 0, up = bit 9 ^ bit 8:
           the port's own directions, the pad's added, put back the same way.
           Took me an age to get that right. */
        unsigned r  = ((v >> 1) & 1) | !!(d & JPF_RIGHT);
        unsigned l  = ((v >> 9) & 1) | !!(d & JPF_LEFT);
        unsigned dn = ((v ^ (v >> 1)) & 1) | !!(d & JPF_DOWN);
        unsigned up = (((v >> 8) ^ (v >> 9)) & 1) | !!(d & JPF_UP);
        return (uint16_t)((v & ~0x0303u) | (r << 1) | (r ^ dn) | (l << 9) | ((l ^ up) << 8));
    }
    if (reg == POTGOR) {
        for (int i = 0; i < 2; i++) {
            uint32_t w = pad(i);
            uint16_t p9 = (uint16_t)(0x0400u << (4 * i));
            if (!w) continue;
            /* a pressed button pulls pin 9 low, whatever POTGO thinks it's driving */
            if ((w >> 28) == TYPE_GAMECTLR && cd32_mode(i)) {
                int s = shifter[i];                /* 8 blue .. 2 play = JPF bits 23 .. 17, in that order */
                if (s == 0 || (s >= 2 && (w & (1u << (15 + s))))) v &= (uint16_t)~p9;
            } else {
                if (w & JPF_BLUE) v &= (uint16_t)~p9;
                if ((w >> 28) == TYPE_MOUSE && (w & JPF_PLAY)) v &= (uint16_t)~(0x0100u << (4 * i));   /* middle button: pin 5 */
            }
        }
    }
    return v;
}

/* CIA-A's PRA as the real CIA answered: red on the fire bits (6: port 0,
   7: port 1). */
uint8_t aga_pad_pra(uint8_t v)
{
    if (!aga_pad_on) return v;
    for (int i = 0; i < 2; i++) {
        uint32_t w = pad(i);
        if (!w || ((w >> 28) == TYPE_GAMECTLR && cd32_mode(i))) continue;   /* the fire line is the clock here, not a button */
        if (w & JPF_RED) v &= (uint8_t)~(0x40 << i);
    }
    return v;
}

/* the 68k's RESET - Kickstart's own at every boot too: the block was the old
   OS's memory, and A314Pad hands out a new one */
void aga_pad_reset(void)
{
    uint32_t a = __atomic_load_n(&pad_addr, __ATOMIC_ACQUIRE);
    if (a) {
        /* I clear its magic on the real bus first. The A314's Pi reads the
           block back before each write and stops for good at one that is not
           as it left it - so it stops now, before the Kickstart that boots
           hands that memory out again. a314d itself only hears of the reset
           once a314.device starts, seconds later (the A314 agent, 30.10.2026). */
        ps_write_32(a, 0);
        aga_pad_register(0);
    }
    ca_os = 1;                                     /* an OS is booting here, not a game */
    regd_read = -1;
    comarea_register(0);                           /* a314.device's too: a new one comes along soon enough */
    potgo = 0;
    shifter[0] = shifter[1] = 8;
    clk_was[0] = clk_was[1] = 0;
}
