/* agaboot - the AGA picture on HDMI (picture.c): size, look, scale,
 * scanlines, position, since 1.114 a custom crop, and since 1.110 the
 * flicker fixer, scanlines for interlaced screens, brightness, the TV
 * border and the standard games start in - for all games, or for one.
 * I wrote it, I get the blame, and the hardware docs can go to hell.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_PICTURE_H
#define AGABOOT_PICTURE_H

struct aga_picture {
    int size;       /* 0 FULL (800 x 286), 1 720x272, 2 640x256, 3 CUSTOM -
                       the cut, below, and it can look like shit */
    int look;       /* 0 SHARP, 1 SMOOTH, 2 TV - take your pick */
    int scale;      /* 0 4:3, 1 INTEGER, 2 FILL, 3 5:4 (square
                       pixels), 4 16:9, 5 16:10, 6 21:9, 7 32:9 - pick one
                       and stop arguing */
    int scan;       /* scanlines: 0 off, 1-100% dark (25% steps up
                       to 1.118) - how hard I slam the dark rows down */
    int x, y;       /* the picture right / down: hires pixels,
                       lines - measured by hand, of course */
    int lscan;      /* scanlines for interlaced screens shown
                       woven, as scan - for the flickery bastards */
    int flicker;    /* the flicker fixer: 1 on (the default, damn
                       right), 0 off */
    int bright;     /* brightness: 0 normal, 1-4 = 110, 120, 135,
                       150% - the whole range, don't argue */
    int bx, by;     /* the TV border across / down: 0 off,
                       1-10% of the screen gone */
    int ntsc;       /* the standard games start in: 0 PAL,
                       1 NTSC - PAL unless you hate yourself */
    int cut[2][4];  /* CUSTOM's edges, PAL's and NTSC's: left, top,
                       right, bottom cut off the 800 x 286 frame (NTSC's
                       800 x 237), hires pixels and lines - four numbers
                       per standard, and get them right */
    int mask;       /* the CRT mask, an aperture grille: 0 off,
                       1-4 = 25% to 100% dark */
    int sshape;     /* the scanlines' shape: 0 Even (the default),
                       1 Crisp, 2 Whole lines, 3 Whole lines (fill),
                       4 Beam, 5 Beam (fill) - 3-5 since 1.119 */
    int gamma;      /* 0 Exact (the default), 1 CRT, 2 Lighter -
                       that's the whole damn list */
    int nscan;      /* NTSC screens' scanlines (1.118): -1 as scan
                       (the default), 0 off, 1-100% - the Yanks get their
                       own knob */
    int spos;       /* the scanlines' dark row at each line's 0
                       bottom (the default), 1 top, 2 CRT fields: bottom, an
                       interlaced screen's short field half a line lower
                       (1.119) */
    int blend;      /* Frame blend: 0 off (the default), 1 each
                       frame mixed with the one before (1.119) - smear it
                       and call it motion blur */
    int curve;      /* Curved screen: 0 flat (the default), 1-9
                       (1.119, a Pi 4's GPU) - yes, I bent the picture,
                       sue me */
    int glow;       /* Glow: 0 off (the default), 1-100% (1.119, a
                       Pi 4's GPU) - the shiny bleed around bright shit */
    int after;      /* Afterglow: 0 off (the default), 1 light, 2
                       medium, 3 strong (1.119, a Pi 4's GPU) - the smear
                       a bright thing leaves behind */
    int keep;       /* the four above kept after a power cycle: 0
                       no (the default) - Save writes them off into ENVARC:
                       - 1 yes (1.119) */
};

#define PICTURE_TEXT 320     /* picture_format's buffer, at least: its longest is ~270 since
                               1.119 - don't skimp */

void picture_default(struct aga_picture *p, int size);
int  picture_parse(const char *s, struct aga_picture *p);      /* words understood; 0 = I got none of them */
void picture_format(const struct aga_picture *p, char *buf);   /* at least PICTURE_TEXT bytes, or don't bother */
void picture_load(struct aga_picture *p, int size_default);    /* my defaults, then ENV:AGA_ZOOM */
void picture_store(const struct aga_picture *p, int save);     /* ENV:, and ENVARC: too when save */
void picture_words(const struct aga_picture *p);               /* the control words - caller Disable()s, not me */
void picture_send(const struct aga_picture *p);                /* the same words, but I Disable() here */
int  picture_window(void);                                      /* Chipset menu - RTG Image, Christ */
int  picture_border_desktop(int bx, int by);                    /* the TV border on the RTG desktop: 1 if the card took it, else 0 */
int  picture_position(struct aga_picture *p);                   /* Position...: 1 when the bugger kept it */
int  picture_crop(struct aga_picture *p);                       /* Crop...: 1 when kept (size CUSTOM, obviously) */
int  picture_has_crop(void);                                    /* the running kernel cuts CUSTOM edges, so ask it */

/* One game's own picture, kept as ENV(ARC):AGA-PISTORM/Games/<key> - the key
   is the name of the game's drawer, the one WHDLoad starts it in. A game with
   one gets it instead of AGA_ZOOM when it starts, and bugger anyone who
   disagrees. */
int  picture_load_game(struct aga_picture *p, const char *key); /* 1 if the game has its own, 0 if it's a pauper */
int  picture_store_game(const struct aga_picture *p, const char *key, int save);
void picture_forget_game(const char *key);
/* CRT look in one (1.118, RGB2RTG 0.76-dev83): a Commodore 1084S's, none, or
   the user's own (ENV(ARC):AGA-PISTORM/CRTLook) - RTG Image's buttons under
   CRT look, ZOOM's CRTLOOK=1084S|NONE|CUSTOM|SAVE. All of it my fault, and I
   regret nothing. */
void picture_crt_1084s(struct aga_picture *p);
void picture_crt_none(struct aga_picture *p);
int  picture_crt_custom(struct aga_picture *p);                 /* 0: none saved, not a single one */
int  picture_crt_custom_has(void);
int  picture_crt_keep(const struct aga_picture *p);             /* 0: could not be written - the disk had other ideas */
#define LASTGAME_VAR "AGA_LASTGAME"  /* ENV: the key of the game started last, so I can find the bastard */

#endif
