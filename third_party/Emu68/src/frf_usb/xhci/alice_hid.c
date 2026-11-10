/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: the parts of a HID report descriptor a mouse needs (HID 1.11,
 * section 6.2.2): which bits of which report hold the buttons, X, Y, the
 * wheel and the horizontal wheel. Written from the HID specification and its
 * usage tables. Short items only (long items are skipped), Push/Pop kept.
 */
#include "alice_hid.h"

#define UP_DESKTOP   0x01
#define UP_BUTTON    0x09
#define UP_CONSUMER  0x0c
#define U_X          0x30
#define U_Y          0x31
#define U_WHEEL      0x38
#define U_AC_PAN     0x238

struct hid_globals { uint32_t page; int32_t lmin; uint32_t rsize, rcount; uint8_t id; };

static uint32_t item_u(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static int32_t item_s(const uint8_t *p, int n)
{
    uint32_t v = item_u(p, n);
    if (n == 1) return (int8_t)v;
    if (n == 2) return (int16_t)v;
    return (int32_t)v;
}

static void take(struct hid_field *f, uint16_t pos, uint32_t size, int sgn, uint8_t id)
{
    if (f->size || size == 0 || size > 32) return;          /* the first one counts */
    f->pos = pos; f->size = (uint8_t)size; f->sgn = (uint8_t)sgn; f->id = id;
}

int hid_mouse_parse(const uint8_t *desc, int len, struct hid_mouse *m)
{
    struct hid_globals g = { 0, 0, 0, 0, 0 }, stack[4];
    int sp = 0;
    uint32_t usage[16], umin = 0, umax = 0;
    int nusage = 0, have_range = 0;
    uint16_t off[256];                                       /* input bits so far, per report ID */
    int xrel = 0;

    *m = (struct hid_mouse){ 0 };
    for (int i = 0; i < 256; i++) off[i] = 0;

    for (int i = 0; i < len; ) {
        uint8_t p = desc[i];
        if (p == 0xfe) {                                    /* long item: size, tag, data */
            if (i + 1 >= len) break;
            i += 3 + desc[i + 1];
            continue;
        }
        int n = (p & 3) == 3 ? 4 : (p & 3), type = (p >> 2) & 3, tag = p >> 4;
        if (i + 1 + n > len) break;
        const uint8_t *d = desc + i + 1;
        i += 1 + n;

        if (type == 1) {                                    /* global */
            switch (tag) {
            case 0: g.page = item_u(d, n); break;
            case 1: g.lmin = item_s(d, n); break;
            case 7: g.rsize = item_u(d, n); break;
            case 8: g.id = (uint8_t)item_u(d, n); m->ids = 1; break;
            case 9: g.rcount = item_u(d, n); break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {                             /* local */
            uint32_t u = item_u(d, n);
            if (n < 4) u |= g.page << 16;                   /* a 4-byte usage carries its own page */
            if (tag == 0 && nusage < 16) usage[nusage++] = u;
            else if (tag == 1) { umin = u; have_range |= 1; }
            else if (tag == 2) { umax = u; have_range |= 2; }
        } else if (type == 0) {                             /* main */
            if (tag == 8) {                                 /* Input */
                uint32_t flags = item_u(d, n);
                uint16_t pos = off[g.id];
                if (!(flags & 1)) {                         /* not Constant: fields with usages */
                    for (uint32_t k = 0; k < g.rcount && k < 64; k++) {
                        uint32_t u;
                        if (nusage) u = usage[k < (uint32_t)nusage ? k : (uint32_t)nusage - 1];
                        else if (have_range == 3 && umin + k <= umax) u = umin + k;
                        else break;
                        uint16_t fp = (uint16_t)(pos + k * g.rsize);
                        uint32_t page = u >> 16, id = u & 0xffff;
                        int sgn = g.lmin < 0;
                        if (page == UP_BUTTON && g.rsize == 1 && id >= 1 && id <= 8) {
                            if (!m->btn.size) { take(&m->btn, fp, 1, 0, g.id); m->nbtn = 0; }
                            if (m->btn.id == g.id && fp == m->btn.pos + m->nbtn) m->nbtn++;
                        } else if (page == UP_DESKTOP && id == U_X) {
                            if (!m->x.size) xrel = (flags & 4) != 0;
                            take(&m->x, fp, g.rsize, sgn, g.id);
                        } else if (page == UP_DESKTOP && id == U_Y) take(&m->y, fp, g.rsize, sgn, g.id);
                        else if (page == UP_DESKTOP && id == U_WHEEL) take(&m->wheel, fp, g.rsize, sgn, g.id);
                        else if (page == UP_CONSUMER && id == U_AC_PAN) take(&m->pan, fp, g.rsize, sgn, g.id);
                    }
                }
                off[g.id] = (uint16_t)(pos + g.rsize * g.rcount);
            }
            nusage = 0; have_range = 0;                     /* locals end with every main item */
        }
    }
    if (m->wheel.size && !m->wheel.sgn) m->wheel.size = 0;  /* an unsigned "wheel" is not one */
    if (m->pan.size && !m->pan.sgn) m->pan.size = 0;
    return m->x.size && m->y.size && xrel && m->x.id == m->y.id;
}

static int32_t get(const uint8_t *r, int len, const struct hid_field *f)
{
    if (!f->size || (int)(f->pos + f->size) > len * 8) return 0;
    uint32_t v = 0;
    for (int b = 0; b < f->size; b++) {
        int bit = f->pos + b;
        v |= (uint32_t)((r[bit >> 3] >> (bit & 7)) & 1) << b;
    }
    if (f->sgn && f->size < 32 && (v >> (f->size - 1)) & 1) v |= ~0u << f->size;
    return (int32_t)v;
}

int hid_mouse_decode(const struct hid_mouse *m, const uint8_t *rep, int len, struct hid_mouse_report *out)
{
    uint8_t id = 0;
    if (m->ids) { if (len < 1) return 0; id = rep[0]; rep++; len--; }
    *out = (struct hid_mouse_report){ 0, 0, 0, 0, 0 };
    int mine = 0;
    if (m->x.id == id) {
        out->dx = get(rep, len, &m->x);
        out->dy = get(rep, len, &m->y);
        mine = 1;
    }
    if (m->btn.size && m->btn.id == id) {
        for (int b = 0; b < m->nbtn; b++) {
            struct hid_field f = m->btn; f.pos = (uint16_t)(m->btn.pos + b);
            if (get(rep, len, &f)) out->buttons |= 1u << b;
        }
        mine = 1;
    }
    if (m->wheel.size && m->wheel.id == id) { out->wheel = get(rep, len, &m->wheel); mine = 1; }
    if (m->pan.size && m->pan.id == id) { out->pan = get(rep, len, &m->pan); mine = 1; }
    return mine;
}
