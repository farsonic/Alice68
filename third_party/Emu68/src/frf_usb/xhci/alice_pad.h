/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: a game pad's (or joystick's) report layout from its HID report
 * descriptor, and its reports turned into what an Amiga joystick port can
 * take: four directions and buttons. Host-independent: the Bluetooth host
 * uses it (src/alice/bt/), a USB driver could; tools/hid-test runs it on the
 * PC against descriptors laid out as real pads' are.
 *
 * Found in the first Generic Desktop Joystick (0x04) or Game Pad (0x05)
 * application collection: X and Y (any size, signed or not), the Hat Switch
 * (0x39, any logical range; outside it = centred), and Button page buttons
 * 1..16, all in one input report (the one X or the hat is in).
 */
#ifndef ALICE_PAD_H
#define ALICE_PAD_H
#include <stdint.h>

struct pad_field {
    uint16_t pos;           /* bit offset in the report, after the report ID byte */
    uint8_t  size;          /* bits, 1..32; 0 = not there */
    uint8_t  sgn;           /* logical minimum < 0: sign-extend */
    int32_t  min, max;      /* logical range */
};

struct hid_pad {
    uint8_t  ok;            /* a pad was found */
    uint8_t  id;            /* its report ID, 0 = none */
    uint8_t  ids;           /* the device numbers its reports */
    uint8_t  nbtn;          /* buttons found (up to 16) */
    struct pad_field x, y, hat;
    uint16_t btn_pos[16];   /* bit of button n+1, 0xffff = none */
};

#define PAD_UP     1u
#define PAD_DOWN   2u
#define PAD_LEFT   4u
#define PAD_RIGHT  8u

struct pad_state {
    unsigned dirs;          /* PAD_* : the stick past 40 % of its way, or the hat */
    unsigned buttons;       /* bit n = button n+1 */
};

/* 1 if desc has a game pad or joystick with X/Y or a hat */
int hid_pad_parse(const uint8_t *desc, int len, struct hid_pad *p);

/* rep: as it came, the report ID first when the device numbers its reports; id: the
   report's ID when the transport gave it apart (BLE notifications), else -1.
   1 if it is the pad's report (out filled), else 0. */
int hid_pad_decode(const struct hid_pad *p, const uint8_t *rep, int len, int id, struct pad_state *out);
#endif
