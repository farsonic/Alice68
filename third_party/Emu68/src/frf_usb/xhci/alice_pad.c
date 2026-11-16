/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: a game pad's layout from its HID report descriptor (alice_pad.h).
 * Written from the USB HID 1.11 specification and its Usage Tables; byte by
 * byte, no structures over the descriptor, so it runs on the PC too.
 */
#include "alice_pad.h"

#define PAD_PG_DESKTOP 0x01u
#define PAD_PG_BUTTON  0x09u
#define PAD_U_JOYSTICK 0x04u
#define PAD_U_GAMEPAD  0x05u
#define PAD_U_X        0x30u
#define PAD_U_Y        0x31u
#define PAD_U_HAT      0x39u

#define PAD_NIDS 16                         /* report IDs whose offsets are followed */
#define PAD_NUSG 24                         /* usages in one main item */

static int32_t pad_sx(uint32_t v, unsigned bytes)      /* a signed item value */
{
    if (bytes == 1) return (int8_t)v;
    if (bytes == 2) return (int16_t)v;
    return (int32_t)v;
}

int hid_pad_parse(const uint8_t *d, int len, struct hid_pad *p)
{
    uint32_t page = 0, rsize = 0, rcount = 0, umin = 0, umax = 0;
    int32_t lmin = 0, lmax = 0;
    unsigned lmax_bytes = 0;
    uint32_t lmax_raw = 0;
    uint8_t id = 0;
    uint32_t usg[PAD_NUSG]; int nusg = 0, have_range = 0;
    uint8_t ids[PAD_NIDS]; uint32_t off[PAD_NIDS]; int nids = 0;
    int depth = 0, paddepth = -1, have_id = 0;
    struct { uint32_t page, rsize, rcount; int32_t lmin, lmax; uint8_t id; } st[4];
    int sp = 0;

    for (int i = 0; i < 16; i++) p->btn_pos[i] = 0xffff;
    p->ok = p->id = p->ids = p->nbtn = 0;
    p->x.size = p->y.size = p->hat.size = 0;

    for (int i = 0; i < len; ) {
        uint8_t b = d[i];
        if (b == 0xfe) {                                /* long item: skipped */
            if (i + 1 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        unsigned n = (b & 3) == 3 ? 4 : (b & 3), type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + (int)n > len) break;
        uint32_t v = 0;
        for (unsigned k = 0; k < n; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        i += 1 + (int)n;
        if (type == 1) {                                /* global */
            switch (tag) {
            case 0: page = v; break;
            case 1: lmin = pad_sx(v, n); break;
            case 2: lmax = pad_sx(v, n); lmax_raw = v; lmax_bytes = n; break;
            case 7: rsize = v; break;
            case 8: id = (uint8_t)v; p->ids = 1; break;
            case 9: rcount = v; break;
            case 10: if (sp < 4) { st[sp].page = page; st[sp].rsize = rsize; st[sp].rcount = rcount; st[sp].lmin = lmin; st[sp].lmax = lmax; st[sp].id = id; sp++; } break;
            case 11: if (sp > 0) { sp--; page = st[sp].page; rsize = st[sp].rsize; rcount = st[sp].rcount; lmin = st[sp].lmin; lmax = st[sp].lmax; id = st[sp].id; } break;
            default: break;
            }
            continue;
        }
        if (type == 2) {                                /* local */
            uint32_t full = n == 4 ? v : (page << 16 | v);
            if (tag == 0 && nusg < PAD_NUSG) usg[nusg++] = full;
            else if (tag == 1) { umin = full; have_range |= 1; }
            else if (tag == 2) { umax = full; have_range |= 2; }
            continue;
        }
        if (type != 0) continue;
        if (tag == 0xa) {                               /* Collection */
            uint32_t u = nusg ? usg[0] : 0;
            if (v == 1 && paddepth < 0 && (u == (PAD_PG_DESKTOP << 16 | PAD_U_GAMEPAD) || u == (PAD_PG_DESKTOP << 16 | PAD_U_JOYSTICK)))
                paddepth = depth;
            depth++;
        } else if (tag == 0xc) {                        /* End Collection */
            depth--;
            if (depth == paddepth) paddepth = -1;
        } else if (tag == 0x8) {                        /* Input */
            int j = 0;
            for (; j < nids && ids[j] != id; j++) ;
            if (j == nids && nids < PAD_NIDS) { ids[nids] = id; off[nids] = 0; nids++; }
            uint32_t at = j < PAD_NIDS ? off[j] : 0;
            int32_t mx = lmax;
            if (mx < lmin && lmax_bytes && lmax_bytes < 4) mx = (int32_t)lmax_raw;   /* 0xff written in one byte: 255 */
            if (paddepth >= 0 && !(v & 1) && (v & 2) && (!have_id || id == p->id)) {
                for (uint32_t e = 0; e < rcount && e < 64; e++) {
                    uint32_t u;
                    if (nusg) u = usg[e < (uint32_t)nusg ? e : (uint32_t)nusg - 1];
                    else if (have_range == 3 && umin + e <= umax) u = umin + e;
                    else break;
                    uint32_t pg = u >> 16, us = u & 0xffff;
                    struct pad_field f = { (uint16_t)(at + e * rsize), (uint8_t)rsize, (uint8_t)(lmin < 0), lmin, mx };
                    if (rsize < 1 || rsize > 32) continue;
                    int took = 0;
                    if (pg == PAD_PG_DESKTOP && us == PAD_U_X && !p->x.size) { p->x = f; took = 1; }
                    else if (pg == PAD_PG_DESKTOP && us == PAD_U_Y && !p->y.size) { p->y = f; took = 1; }
                    else if (pg == PAD_PG_DESKTOP && us == PAD_U_HAT && !p->hat.size) { p->hat = f; took = 1; }
                    else if (pg == PAD_PG_BUTTON && us >= 1 && us <= 16 && rsize == 1 && p->btn_pos[us - 1] == 0xffff) {
                        p->btn_pos[us - 1] = (uint16_t)f.pos;
                        if (us > p->nbtn) p->nbtn = (uint8_t)us;
                        took = 1;
                    }
                    if (took && !have_id) { have_id = 1; p->id = id; }
                }
            }
            if (j < PAD_NIDS) off[j] = at + rsize * rcount;
        }
        nusg = 0; have_range = 0; umin = umax = 0;          /* locals end with each main item */
    }
    p->ok = (uint8_t)((p->x.size && p->y.size) || p->hat.size);
    return p->ok;
}

static uint32_t pad_bits(const uint8_t *r, int n, unsigned pos, unsigned size)
{
    uint32_t v = 0;
    for (unsigned b = 0; b < size; b++) {
        unsigned at = pos + b;
        if ((int)(at / 8) < n && (r[at / 8] >> (at % 8) & 1)) v |= 1u << b;
    }
    return v;
}
static int32_t pad_value(const struct pad_field *f, const uint8_t *r, int n)
{
    uint32_t v = pad_bits(r, n, f->pos, f->size);
    if (f->sgn && f->size < 32 && (v >> (f->size - 1) & 1)) v |= ~0u << f->size;
    return (int32_t)v;
}
/* -1 below 40 % of the way to the minimum, +1 past 40 % to the maximum, else 0 */
static int pad_axis(const struct pad_field *f, const uint8_t *r, int n)
{
    if (!f->size || f->max <= f->min) return 0;
    int64_t v2 = 2 * (int64_t)pad_value(f, r, n) - ((int64_t)f->min + f->max), range = (int64_t)f->max - f->min;
    if (v2 * 10 < -4 * range) return -1;
    if (v2 * 10 > 4 * range) return 1;
    return 0;
}

int hid_pad_decode(const struct hid_pad *p, const uint8_t *rep, int len, int id, struct pad_state *out)
{
    if (!p->ok || !rep) return 0;
    if (p->ids) {
        if (id < 0) {
            if (len < 1 || rep[0] != p->id) return 0;
            rep++; len--;
        } else if (id != p->id) return 0;
    }
    unsigned dirs = 0;
    int ax = pad_axis(&p->x, rep, len), ay = pad_axis(&p->y, rep, len);
    if (ax < 0) dirs |= PAD_LEFT;
    if (ax > 0) dirs |= PAD_RIGHT;
    if (ay < 0) dirs |= PAD_UP;
    if (ay > 0) dirs |= PAD_DOWN;
    if (p->hat.size) {
        int32_t v = pad_value(&p->hat, rep, len), n = p->hat.max - p->hat.min + 1;
        if (v >= p->hat.min && v <= p->hat.max) {
            static const uint8_t eight[8] = { PAD_UP, PAD_UP | PAD_RIGHT, PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
                                              PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT, PAD_UP | PAD_LEFT };
            static const uint8_t four[4] = { PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT };
            int k = v - p->hat.min;
            if (n == 8) dirs |= eight[k];
            else if (n == 4) dirs |= four[k];
        }
    }
    unsigned btn = 0;
    for (int i = 0; i < 16; i++)
        if (p->btn_pos[i] != 0xffff && pad_bits(rep, len, p->btn_pos[i], 1)) btn |= 1u << i;
    out->dirs = dirs;
    out->buttons = btn;
    return 1;
}
