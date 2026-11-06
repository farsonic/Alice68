/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 kernel WiFi: the FullMAC protocol (see fmac.h). Host-independent.
 * Layouts follow Linux brcmfmac's fwil_types.h/fweh.h and OpenBSD's
 * bwfmreg.h (ISC); offsets are the firmware's natural 4-byte alignment. */
#include "fmac.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

static void cpy(uint8_t *d, const void *s, unsigned n)
{
    const uint8_t *p = s;
    if (d < p) { while (n--) *d++ = *p++; }
    else if (d > p) { while (n--) d[n] = p[n]; }
}

/* ---- SDPCM ---------------------------------------------------------------- */
int sdpcm_parse(const uint8_t *h, struct sdpcm_rx *r)
{
    uint16_t len = fm_le16(h), ck = fm_le16(h + 2);
    if (len == 0 && ck == 0) return 1;
    if ((uint16_t)(len ^ ck) != 0xffff) return -1;
    if (len < SDPCM_HDRLEN) return -2;
    r->len = len;
    r->seq = h[4];
    r->chan = h[5] & 0x0f;
    r->nextlen = (unsigned)h[6] << 4;
    r->dataoff = h[7];
    r->flowctl = h[8];
    r->maxseq = h[9];
    if (r->dataoff < SDPCM_HDRLEN || r->dataoff > len) return -3;
    return 0;
}

unsigned sdpcm_frame(uint8_t *buf, unsigned chan, uint8_t seq, unsigned payload)
{
    unsigned len = SDPCM_HDRLEN + payload;
    if (chan == SDPCM_CHAN_DATA) {
        len += BCDC_DATA_HDR;
        buf[12] = BCDC_PROTO_VER << 4;   /* flags: version */
        buf[13] = 0;                     /* priority */
        buf[14] = 0;                     /* flags2: interface 0 */
        buf[15] = 0;                     /* data offset, words */
    }
    fm_put16(buf, (uint16_t)len);
    fm_put16(buf + 2, (uint16_t)~len);
    buf[4] = seq;
    buf[5] = (uint8_t)chan;
    buf[6] = 0;                          /* nextlen */
    buf[7] = SDPCM_HDRLEN;               /* data offset */
    buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
    unsigned padded = sdpcm_padded(len);
    for (unsigned i = len; i < padded; i++) buf[i] = 0;
    return padded;
}

/* ---- BCDC ----------------------------------------------------------------- */
unsigned bcdc_dcmd(uint8_t *buf, uint32_t cmd, int set, uint16_t reqid, const void *payload, unsigned len)
{
    if (payload && payload != buf + BCDC_DCMD_HDR) cpy(buf + BCDC_DCMD_HDR, payload, len);
    fm_put32(buf, cmd);
    fm_put32(buf + 4, len);
    fm_put32(buf + 8, (uint32_t)reqid << 16 | (set ? BCDC_SET : 0));   /* interface 0 */
    fm_put32(buf + 12, 0);
    return BCDC_DCMD_HDR + len;
}

unsigned bcdc_iovar(uint8_t *buf, unsigned max, const char *name, const void *val, unsigned len)
{
    unsigned n = 0;
    while (name[n]) n++;
    if (n + 1 + len > max) return 0;
    if (val) cpy(buf + n + 1, val, len);       /* value first: it may sit where the name goes */
    cpy(buf, name, n + 1);
    return n + 1 + len;
}

int bcdc_parse_reply(const uint8_t *m, unsigned len, struct bcdc_reply *r)
{
    if (len < BCDC_DCMD_HDR) return -1;
    uint32_t fl = fm_le32(m + 8);
    r->cmd = fm_le32(m);
    r->reqid = (uint16_t)(fl >> 16);
    r->error = fl & BCDC_ERROR;
    r->status = (int32_t)fm_le32(m + 12);
    unsigned dl = fm_le32(m + 4);
    r->data = m + BCDC_DCMD_HDR;
    r->len = dl < len - BCDC_DCMD_HDR ? dl : len - BCDC_DCMD_HDR;
    return 0;
}

const uint8_t *bcdc_data(const uint8_t *p, unsigned len, unsigned *outlen)
{
    if (len < BCDC_DATA_HDR || (p[0] >> 4) != BCDC_PROTO_VER) return 0;
    unsigned off = BCDC_DATA_HDR + (unsigned)p[3] * 4;
    if (off > len) return 0;
    *outlen = len - off;
    return p + off;
}

/* ---- events --------------------------------------------------------------- */
int fm_event_parse(const uint8_t *f, unsigned len, struct fm_event *e)
{
    if (len < 72 || fm_be16(f + 12) != 0x886c) return 0;
    if (f[19] != 0x00 || f[20] != 0x10 || f[21] != 0x18 || fm_be16(f + 22) != 1) return 0;
    const uint8_t *m = f + 24;              /* brcmf_event_msg_be: big-endian */
    e->flags = fm_be16(m + 2);
    e->type = fm_be32(m + 4);
    e->status = fm_be32(m + 8);
    e->reason = fm_be32(m + 12);
    unsigned dl = fm_be32(m + 20);
    memcpy(e->addr, m + 24, 6);
    e->data = f + 72;
    e->datalen = dl < len - 72 ? dl : len - 72;
    return 1;
}

/* ---- scanning ------------------------------------------------------------- */
unsigned fm_escan_params(uint8_t *b, uint16_t sync_id)
{
    memset(b, 0, 72);
    fm_put32(b, 1);                 /* escan version */
    fm_put16(b + 4, 1);             /* action: start */
    fm_put16(b + 6, sync_id);
    uint8_t *p = b + 8;             /* brcmf_scan_params_le */
    memset(p + 36, 0xff, 6);        /* bssid: broadcast */
    p[42] = 2;                      /* bss type: any */
    p[43] = 0;                      /* active scan */
    fm_put32(p + 44, 0xffffffffu);  /* nprobes: default */
    fm_put32(p + 48, 0xffffffffu);  /* active time */
    fm_put32(p + 52, 0xffffffffu);  /* passive time */
    fm_put32(p + 56, 0xffffffffu);  /* home time */
    fm_put32(p + 60, 0);            /* all channels, one (wildcard) SSID */
    return 8 + 64;
}

static int suite(const uint8_t *s, const uint8_t *oui) { return s[0] == oui[0] && s[1] == oui[1] && s[2] == oui[2] ? s[3] : -1; }

/* RSN element body (rsn = 1) or WPA vendor element body after its OUI type (rsn = 0) */
static void parse_rsn(const uint8_t *p, unsigned n, int rsn, struct fm_bss *b)
{
    static const uint8_t ieee[3] = { 0x00, 0x0f, 0xac }, msft[3] = { 0x00, 0x50, 0xf2 };
    const uint8_t *oui = rsn ? ieee : msft;
    if (n < 2 || fm_le16(p) != 1) return;
    p += 2; n -= 2;
    int gc = 4, pc = 4;                         /* defaults when the element is cut short */
    if (n >= 4) { gc = suite(p, oui); p += 4; n -= 4; }
    if (n >= 2) {
        unsigned c = fm_le16(p); p += 2; n -= 2;
        pc = -1;
        for (unsigned i = 0; i < c && n >= 4; i++, p += 4, n -= 4) {
            int t = suite(p, oui);
            if (t == 4 || (t == 2 && pc != 4)) pc = t;
        }
    }
    int akm_psk = 0, akm_sae = 0, akm_1x = 0;
    if (n >= 2) {
        unsigned c = fm_le16(p); p += 2; n -= 2;
        for (unsigned i = 0; i < c && n >= 4; i++, p += 4, n -= 4) {
            int t = suite(p, oui);
            if (t == 2 || t == 6) akm_psk = 1;
            else if (t == 8) akm_sae = 1;
            else if (t == 1 || t == 5) akm_1x = 1;
        }
    } else
        akm_psk = 1;
    if (rsn && n >= 2) {
        uint16_t cap = fm_le16(p);
        b->mfp = (cap & 0x40) ? MFP_REQUIRED : (cap & 0x80) ? MFP_CAPABLE : MFP_NONE;
    }
    if (gc < 0 || pc < 0) return;
    if (rsn) {
        if (akm_psk) b->security |= SEC_WPA2;
        if (akm_sae) b->security |= SEC_WPA3;
    } else if (akm_psk)
        b->security |= SEC_WPA;
    if (akm_1x) b->security |= SEC_8021X;
    /* RSN wins over WPA for the ciphers when both are offered */
    if (rsn || !(b->security & (SEC_WPA2 | SEC_WPA3))) { b->group_cipher = (uint8_t)gc; b->pairwise = (uint8_t)pc; }
}

void fm_parse_ies(const uint8_t *ie, unsigned len, struct fm_bss *b)
{
    while (len >= 2 && (unsigned)ie[1] + 2 <= len) {
        unsigned id = ie[0], n = ie[1];
        if (id == 48)
            parse_rsn(ie + 2, n, 1, b);
        else if (id == 221 && n >= 4 && ie[2] == 0x00 && ie[3] == 0x50 && ie[4] == 0xf2 && ie[5] == 1)
            parse_rsn(ie + 6, n - 4, 0, b);
        ie += n + 2;
        len -= n + 2;
    }
}

int fm_escan_parse(const uint8_t *d, unsigned len, void (*fn)(void *, const struct fm_bss *), void *ctx)
{
    if (len < 12) return -1;
    unsigned buflen = fm_le32(d), count = fm_le16(d + 10);
    if (buflen < len) len = buflen;
    unsigned off = 12;
    int n = 0;
    for (unsigned i = 0; i < count; i++) {
        if (off + 128 > len) return -2;
        const uint8_t *bi = d + off;
        unsigned blen = fm_le32(bi + 4);
        if (blen < 128 || off + blen > len) return -3;
        struct fm_bss b;
        memset(&b, 0, sizeof b);
        memcpy(b.bssid, bi + 8, 6);
        b.ssid_len = bi[18] > 32 ? 32 : bi[18];
        memcpy(b.ssid, bi + 19, b.ssid_len);
        b.ssid[b.ssid_len] = 0;
        b.chanspec = fm_le16(bi + 72);
        b.rssi = (int16_t)fm_le16(bi + 78);
        b.channel = bi[88] ? bi[88] : (uint8_t)fm_chanspec_channel(b.chanspec);
        unsigned ieoff = fm_le16(bi + 116), ielen = fm_le32(bi + 120);
        if (ieoff >= 128 && ieoff + ielen <= blen) fm_parse_ies(bi + ieoff, ielen, &b);
        if (!(b.security & (SEC_WPA | SEC_WPA2 | SEC_WPA3 | SEC_8021X)))
            b.security |= (fm_le16(bi + 16) & 0x10) ? SEC_WEP : SEC_OPEN;   /* privacy bit */
        if (fn) fn(ctx, &b);
        n++;
        off += blen;
    }
    return n;
}

/* ---- joining -------------------------------------------------------------- */
unsigned fm_build_wpaie(uint8_t *p, const struct fm_bss *b, int sae)
{
    uint8_t gc = b->group_cipher ? b->group_cipher : 4, pc = b->pairwise ? b->pairwise : 4;
    if ((b->security & (SEC_WPA2 | SEC_WPA3)) || sae) {
        static const uint8_t o[3] = { 0x00, 0x0f, 0xac };
        uint8_t *q = p;
        *q++ = 48; *q++ = 20;
        *q++ = 1; *q++ = 0;
        cpy(q, o, 3); q[3] = gc; q += 4;
        *q++ = 1; *q++ = 0; cpy(q, o, 3); q[3] = pc; q += 4;
        *q++ = 1; *q++ = 0; cpy(q, o, 3); q[3] = sae ? 8 : 2; q += 4;
        *q++ = sae ? 0xc0 : 0x00; *q++ = 0;           /* RSN capabilities: MFP required for SAE */
        return (unsigned)(q - p);
    }
    static const uint8_t o[3] = { 0x00, 0x50, 0xf2 };
    uint8_t *q = p;
    *q++ = 221; *q++ = 22;
    cpy(q, o, 3); q[3] = 1; q += 4;
    *q++ = 1; *q++ = 0;
    cpy(q, o, 3); q[3] = gc; q += 4;
    *q++ = 1; *q++ = 0; cpy(q, o, 3); q[3] = pc; q += 4;
    *q++ = 1; *q++ = 0; cpy(q, o, 3); q[3] = 2; q += 4;
    return (unsigned)(q - p);
}

static void ssid_le(uint8_t *p, const char *ssid, unsigned n)
{
    if (n > 32) n = 32;
    memset(p, 0, 36);
    fm_put32(p, n);
    cpy(p + 4, ssid, n);
}

unsigned fm_join_params(uint8_t *b, const char *ssid, unsigned n, const uint8_t *bssid, uint16_t chanspec)
{
    memset(b, 0, 72);
    ssid_le(b, ssid, n);
    b[36] = 0xff;                           /* scan type: default */
    int known = bssid && chanspec;
    fm_put32(b + 40, known ? 320 / 20 : 0xffffffffu);   /* nprobes */
    fm_put32(b + 44, known ? 320 : 0xffffffffu);        /* active dwell, ms */
    fm_put32(b + 48, known ? 400 : 0xffffffffu);        /* passive dwell */
    fm_put32(b + 52, 0xffffffffu);                      /* home time */
    if (bssid) cpy(b + 56, bssid, 6); else memset(b + 56, 0xff, 6);
    fm_put32(b + 64, known ? 1 : 0);
    fm_put16(b + 68, known ? chanspec : 0);
    return 72;
}

unsigned fm_ssid_params(uint8_t *b, const char *ssid, unsigned n, const uint8_t *bssid)
{
    memset(b, 0, 52);
    ssid_le(b, ssid, n);
    if (bssid) cpy(b + 36, bssid, 6); else memset(b + 36, 0xff, 6);
    return 52;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

unsigned fm_pmk(uint8_t *b, const char *pass, unsigned n, int short_form)
{
    unsigned keymax = short_form ? 65 : 128, size = short_form ? 70 : 132;
    memset(b, 0, size);
    int hex = n == 64;
    for (unsigned i = 0; hex && i < 64; i++) if (hexval(pass[i]) < 0) hex = 0;
    if (hex) {                              /* 64 hex digits: the PMK itself, as binary (flags 0) */
        for (unsigned i = 0; i < 32; i++) b[4 + i] = (uint8_t)(hexval(pass[2 * i]) << 4 | hexval(pass[2 * i + 1]));
        fm_put16(b, 32);
        return size;
    }
    if (n > keymax - 1) n = keymax - 1;
    fm_put16(b, (uint16_t)n);
    fm_put16(b + 2, 1);                     /* WSEC_PASSPHRASE */
    cpy(b + 4, pass, n);
    return size;
}

unsigned fm_sae_password(uint8_t *b, const char *pass, unsigned n)
{
    if (n > 127) n = 127;
    memset(b, 0, 130);
    fm_put16(b, (uint16_t)n);
    cpy(b + 2, pass, n);
    return 130;
}

unsigned fm_country(uint8_t *b, const char *cc)
{
    memset(b, 0, 12);
    b[0] = b[8] = (uint8_t)cc[0];
    b[1] = b[9] = (uint8_t)cc[1];
    return 12;                              /* revision 0 */
}

/* ---- NVRAM ---------------------------------------------------------------- */
unsigned fm_nvram(const uint8_t *in, unsigned len, uint8_t *out)
{
    unsigned o = 0, count = 0;
    int skip = 0;
    for (unsigned i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == '\n') {
            if (count) out[o++] = 0;
            count = 0; skip = 0;
            continue;
        }
        if (skip || c == '\r') continue;
        if (count == 0 && (c == ' ' || c == '\t')) continue;   /* leading blanks */
        if (c == '#' && count == 0) { skip = 1; continue; }
        out[o++] = c;
        count++;
    }
    if (count) out[o++] = 0;
    out[o++] = 0;                           /* the list ends with an empty entry */
    while (o & 3) out[o++] = 0;
    uint32_t words = o / 4, token = (words & 0xffff) | (~words << 16);
    fm_put32(out + o, token);
    return o + 4;
}

/* ---- firmware names ------------------------------------------------------- */
const char *fm_fw_stem(uint16_t chip, uint8_t rev, int alt)
{
    if (chip == 0x4345) {
        if (rev == 9) return alt ? "cyfmac43456-sdio" : "brcmfmac43456-sdio";   /* CM4 */
        return alt ? "brcmfmac43455-sdio" : "cyfmac43455-sdio";                 /* Pi 4, Pi 3B+ */
    }
    if (chip == 43430 || chip == 0xa9a6) return alt ? "cyfmac43430-sdio" : "brcmfmac43430-sdio";
    return 0;
}
