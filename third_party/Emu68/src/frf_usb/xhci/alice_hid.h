/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: a USB mouse's report layout from its HID report descriptor, so the
 * kernel can read the scroll wheel (boot protocol, which frf_xhci.c used for
 * every mouse, has buttons and X/Y only). Host-independent: tools/hid-test
 * runs it on the PC against real mice's descriptors.
 */
#ifndef ALICE_HID_H
#define ALICE_HID_H
#include <stdint.h>

struct hid_field {
    uint16_t pos;           /* bit offset in the report, after the report ID byte */
    uint8_t  size;          /* bits, 1..32; 0 = not there */
    uint8_t  sgn;           /* logical minimum < 0: sign-extend */
    uint8_t  id;            /* report ID, 0 = none */
};

struct hid_mouse {
    uint8_t  ids;           /* the device numbers its reports (first byte = report ID) */
    uint8_t  nbtn;          /* buttons in btn (1 bit each), up to 8 */
    struct hid_field btn;   /* the first button bit; size 1 */
    struct hid_field x, y;  /* relative X and Y (required) */
    struct hid_field wheel; /* Generic Desktop Wheel: + = away from the user (scroll up) */
    struct hid_field pan;   /* Consumer AC Pan: + = right */
};

/* 1 if desc has a relative X and Y (and so can be used in report protocol), else 0 */
int hid_mouse_parse(const uint8_t *desc, int len, struct hid_mouse *m);

/* a report from the interrupt endpoint: 1 if it is the mouse's (out filled), else 0 */
struct hid_mouse_report { int dx, dy, wheel, pan; unsigned buttons; };
int hid_mouse_decode(const struct hid_mouse *m, const uint8_t *rep, int len, struct hid_mouse_report *out);
#endif
