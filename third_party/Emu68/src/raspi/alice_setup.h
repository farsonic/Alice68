/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 setup screen: the options in cmdline.txt, shown and changed before
 * AmigaOS starts (Del or F2 while the splash asks for it).
 *
 * Host-independent: the settings come in as the kernel's bootargs, keys as
 * USB HID usages, the screen is drawn with alice_splash.c's primitives, and
 * what changed goes out as an edit list for alice68hdf.device, which writes
 * cmdline.txt (amiga/hdf/cmdedit.c):
 *     -KEY      drop the options named KEY ("hdf" drops hdf=off, "hdf.readonly" itself)
 *     +OPTION   add OPTION at the end ("aga.scale=4", "wifi.ssid=\"My Net\"")
 * one per line. Options it doesn't know stay as they were.
 * tools/splash-preview renders it on a PC; tools/setup-test checks the logic.
 */
#ifndef ALICE_SETUP_H
#define ALICE_SETUP_H
#include <stdint.h>
#include "alice_splash.h"

#define AS_ITEMS    24
#define AS_TEXT     64
#define AS_INFO     8

enum { AS_NONE = 0, AS_REDRAW, AS_SAVE, AS_EXIT };

struct as_state {
    int sec, row;                       /* the section shown, the row selected in it */
    int editing;                        /* a text option being typed */
    int dialog;                         /* 0 none, 1 save and restart?, 2 leave without saving? */
    uint8_t cur[AS_ITEMS], orig[AS_ITEMS];          /* choices */
    char text[AS_ITEMS][AS_TEXT], otext[AS_ITEMS][AS_TEXT];
    char edit[AS_TEXT];                 /* what is being typed */
    struct al_info info[AS_INFO];       /* the System section's lines */
    unsigned ninfo;
};

/* the options as bootargs has them; info: read-only lines for the System section */
void as_init(struct as_state *s, const char *bootargs, const struct al_info *info, unsigned ninfo);
/* a key pressed: HID usage, modifier byte. AS_SAVE: the user confirmed saving;
   AS_EXIT: leave, nothing to save; AS_REDRAW: draw again */
int  as_key(struct as_state *s, uint8_t usage, uint8_t mods);
/* the whole screen (al_setup() first) */
void as_draw(const struct as_state *s);
/* a message in the middle of the screen, over whatever is there ("Saving ...") */
void as_message(const char *title, const char *text);
/* the edit list for what changed (see above); its length, 0 nothing changed, -1 too long */
int  as_edits(const struct as_state *s, char *out, unsigned max);
int  as_changed(const struct as_state *s);
#endif
