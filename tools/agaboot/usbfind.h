/* usbfind.c: the PiStorm USB's mouse and pad, found through the USB stack
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_USBFIND_H
#define AGABOOT_USBFIND_H

#include <exec/types.h>

/* One HID device as found: where it is plugged in, its interrupt IN
   endpoint, and its report's layout - bit offsets from the report's first
   byte, its ID included. Everything I need and nothing I don't. */
struct usbf {
    UBYTE found;
    UBYTE root, dci, id;                           /* the xHCI root port, the endpoint's DCI, the report ID (0 none) */
    ULONG route;                                   /* the route string below the root port (xHCI 4.3.3) */
    UBYTE addr;                                    /* the stack's own address - shown, never sent; don't confuse the two */
    char name[48];
    UWORD x_off, y_off;                            /* X and Y: a mouse's moves, a pad's stick */
    UBYTE x_bits, y_bits;
    LONG x_min, x_max, y_min, y_max;
    UWORD hat_off;                                 /* a pad's hat switch */
    UBYTE hat_bits;
    BYTE hat_min;
    UBYTE nbtn;                                    /* buttons 1..nbtn, each one bit */
    UWORD btn[16];
};

int usbf_find(struct usbf *mouse, struct usbf *pad, int verbose);   /* 0: no USB stack to ask, so don't expect any */

#endif
