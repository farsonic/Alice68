/* agaboot - Chipset menu - Expert - Controller (pad.c): the gamepads in
 * WHDLoad games, their tester, and the button that jumps - the whole
 * miserable contract in one header
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_PAD_H
#define AGABOOT_PAD_H

/* the jump button: 0 none (Up on the pad, as a joystick), 1-7 A B X Y LB RB
   Start; a game's whole setting packs it with the button the game jumps with,
   the same numbers (0 Up), and what Up on the pad then does - presses it too,
   only it (SET_ONLY: no Up), or nothing but Up (SET_PLAIN: only the jump
   button presses it), and what a pad is to the game - 0 a CD32 pad, 1 a
   2-button stick, 2 a 1-button one. Seven bits of pure spite, all in one
   int: */
#define SET_JUMP_M 0x07
#define SET_AS_M   0x70
#define SET_ONLY   0x80
#define SET_PLAIN  0x100
#define SET_MODE_M 0x600
#define SET_JUMP(s) ((s) & 7)
#define SET_AS(s)   (((s) >> 4) & 7)
#define SET_MODE(s) (((s) >> 9) & 3)
int  pad_parse_jump(const char *w);             /* "B", "start": index, -1 if it's gibberish */
const char *pad_jump_word(int n);               /* 0 -> "UP", of course */
const char *pad_jump_name(int n);               /* 2 -> "B (blue)", in case you wondered */
int  pad_jump_for(const char *game);            /* the setting: the game's own, else all games' (D-PAD Jump
                                                   while it's on, else none); NULL or "" means all games' */
int  pad_has_own(const char *game);
void pad_store(const char *game, int s, int save);   /* NULL or "": D-PAD Jump itself */
int  pad_preset(void);                          /* D-PAD Jump, as saved or as it comes out of the box */
int  pad_dpad_on(void);                         /* ...whether it's on in all games */
void pad_store_dpad(int on, int save);
void pad_forget(const char *game);              /* the game gets all games' again, no trace left */
void pad_send(int s);                           /* to the kernel: the whole setting, no half measures */
void pad_entry(const char *game);               /* SANDBOX ON: the game's setting, or all games' */

/* the A314's pads and mouse in games, or the Amiga's own ports alone */
int  pad_use_a314(void);                        /* A314=ON in ENV:AGA_PAD gives 1, else 0 */
void pad_store_a314(int on, int save);
void pad_send_a314(int on);                     /* to the kernel, same as the rest */
int  pad_use_usb(void);                         /* USB=ON in ENV:AGA_PAD gives 1 (usbmouse.c's devices) */
void pad_store_usb(int on, int save);

int  pad_window(void);                          /* agaboot PAD ASK: 0 Cancel, 1 Use, 2 Save - my kind of API */
int  pad_command(int argc, char **argv);        /* agaboot PAD ...: the command line's problem child */

#endif
