/* SPDX-License-Identifier: GPL-2.0-only
 * PC test of amiga/wifi/fmac.c, the FullMAC protocol layer of alice68wifi.device:
 * SDPCM framing and credits, BCDC ioctls and data headers, firmware events,
 * escan results and their security elements, the join/PMK/country messages
 * and the NVRAM conversion, against byte layouts written out by hand from
 * Linux brcmfmac's structures. */
#include <stdio.h>
#include <string.h>
#include "fmac.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void t_sdpcm(void)
{
    uint8_t b[2048];
    memset(b, 0xee, sizeof b);
    memcpy(b + 12, "abc", 3);
    unsigned n = sdpcm_frame(b, SDPCM_CHAN_CONTROL, 7, 3);
    CHECK(n == 16, "control frame padded to %u", n);
    CHECK(fm_le16(b) == 15 && fm_le16(b + 2) == (uint16_t)~15, "length/checksum");
    CHECK(b[4] == 7 && b[5] == 0 && b[7] == 12 && b[15] == 0, "sw header / pad");
    struct sdpcm_rx r;
    CHECK(sdpcm_parse(b, &r) == 0 && r.len == 15 && r.chan == 0 && r.dataoff == 12 && r.seq == 7, "parse back");
    n = sdpcm_frame(b, SDPCM_CHAN_DATA, 1, 1000);
    CHECK(n == 1024, "1016-byte data frame -> %u (whole blocks)", n);
    CHECK(b[12] == 0x20 && b[15] == 0, "BCDC data header");
    n = sdpcm_frame(b, SDPCM_CHAN_DATA, 1, 512 - 16);
    CHECK(n == 512, "exactly one block -> %u", n);
    memset(b, 0, 12);
    CHECK(sdpcm_parse(b, &r) == 1, "empty header = no frame");
    fm_put16(b, 20); fm_put16(b + 2, 0x1234);
    CHECK(sdpcm_parse(b, &r) < 0, "bad checksum refused");
    fm_put16(b, 8); fm_put16(b + 2, (uint16_t)~8);
    CHECK(sdpcm_parse(b, &r) < 0, "short frame refused");
    fm_put16(b, 100); fm_put16(b + 2, (uint16_t)~100); b[6] = 5; b[7] = 14; b[9] = 0x20;
    CHECK(sdpcm_parse(b, &r) == 0 && r.nextlen == 80 && r.dataoff == 14 && r.maxseq == 0x20, "nextlen/maxseq");
    CHECK(sdpcm_credit(10, 12) && !sdpcm_credit(12, 12) && !sdpcm_credit(20, 10) && sdpcm_credit(250, 3), "credits incl. wrap");
}

static void t_bcdc(void)
{
    uint8_t b[256];
    unsigned n = bcdc_iovar(b + 16, sizeof b - 16, "mpc", "\x01\0\0\0", 4);
    CHECK(n == 8 && !memcmp(b + 16, "mpc\0\x01\0\0\0", 8), "iovar");
    n = bcdc_dcmd(b, WLC_SET_VAR, 1, 0x1234, b + 16, n);
    CHECK(n == 24 && fm_le32(b) == 263 && fm_le32(b + 4) == 8 && fm_le32(b + 8) == 0x12340002, "dcmd header");
    /* a reply: error flag and status */
    fm_put32(b + 8, 0x12340001); fm_put32(b + 12, (uint32_t)-23);
    struct bcdc_reply r;
    CHECK(bcdc_parse_reply(b, n, &r) == 0 && r.reqid == 0x1234 && r.error && r.status == -23 && r.len == 8, "reply");
    /* name and value overlapping: value moved first */
    uint8_t v[16] = "XYZW";
    n = bcdc_iovar(v, sizeof v, "ab", v, 4);
    CHECK(n == 7 && !memcmp(v, "ab\0XYZW", 7), "overlapping iovar");
    uint8_t d[8] = { 0x20, 0, 0, 1, 9, 9, 9, 9 };
    unsigned l;
    const uint8_t *p = bcdc_data(d, 8, &l);
    CHECK(p == d + 8 && l == 0, "data offset in words");
    d[3] = 0;
    p = bcdc_data(d, 8, &l);
    CHECK(p == d + 4 && l == 4, "data payload");
    d[0] = 0x10;
    CHECK(!bcdc_data(d, 8, &l), "wrong BCDC version refused");
}

static void t_event(void)
{
    uint8_t f[100];
    memset(f, 0, sizeof f);
    f[12] = 0x88; f[13] = 0x6c;
    f[14] = 0x80; f[15] = 0x01;                     /* vendor long */
    f[19] = 0x00; f[20] = 0x10; f[21] = 0x18; f[23] = 1;
    uint8_t *m = f + 24;
    m[1] = 2; m[3] = 1;                             /* version 2, flags LINK */
    m[7] = E_LINK; m[11] = 0; m[15] = 3;            /* type, status, reason 3 */
    m[23] = 4;                                      /* datalen */
    memcpy(m + 24, "\x11\x22\x33\x44\x55\x66", 6);
    struct fm_event e;
    CHECK(fm_event_parse(f, sizeof f, &e) == 1 && e.type == E_LINK && e.flags == 1 && e.reason == 3 && e.datalen == 4 &&
          e.data == f + 72 && e.addr[5] == 0x66, "event");
    f[13] = 0x00;                                   /* IPv4 */
    CHECK(fm_event_parse(f, sizeof f, &e) == 0, "ordinary frame is not an event");
    uint8_t mask[EVENT_MASK_LEN] = { 0 };
    fm_event_mask_set(mask, E_ESCAN_RESULT);
    CHECK(mask[8] == 0x20, "event mask bit 69");
}

/* escan result with one bss_info record (128 bytes + IEs) */
static unsigned mk_escan(uint8_t *d, const char *ssid, const uint8_t *ies, unsigned ielen, uint16_t cap, int16_t rssi)
{
    memset(d, 0, 12 + 128 + ielen);
    uint8_t *bi = d + 12;
    fm_put32(bi, 109);
    fm_put32(bi + 4, 128 + ielen);
    memcpy(bi + 8, "\xaa\xbb\xcc\xdd\xee\x01", 6);
    fm_put16(bi + 16, cap);
    bi[18] = (uint8_t)strlen(ssid);
    memcpy(bi + 19, ssid, strlen(ssid));
    fm_put16(bi + 72, 0x1006);                      /* 2.4 GHz, 20 MHz, channel 6 */
    fm_put16(bi + 78, (uint16_t)rssi);
    bi[88] = 6;
    fm_put16(bi + 116, 128);
    fm_put32(bi + 120, ielen);
    memcpy(bi + 128, ies, ielen);
    fm_put32(d, 12 + 128 + ielen);
    fm_put32(d + 4, 109);
    fm_put16(d + 10, 1);
    return 12 + 128 + ielen;
}
static struct fm_bss got;
static void take(void *ctx, const struct fm_bss *b) { (void)ctx; got = *b; }

static void t_scan(void)
{
    uint8_t p[80];
    CHECK(fm_escan_params(p, 0x55) == 72, "escan params length");
    CHECK(fm_le32(p) == 1 && fm_le16(p + 4) == 1 && fm_le16(p + 6) == 0x55 && p[8 + 36] == 0xff && p[8 + 42] == 2 &&
          fm_le32(p + 8 + 44) == 0xffffffffu && fm_le32(p + 8 + 60) == 0, "escan params layout");

    uint8_t d[400];
    static const uint8_t rsn_psk[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0x80, 0 };
    unsigned n = mk_escan(d, "Home", rsn_psk, sizeof rsn_psk, 0x0411, -55);
    CHECK(fm_escan_parse(d, n, take, 0) == 1, "one BSS");
    CHECK(!strcmp(got.ssid, "Home") && got.rssi == -55 && got.channel == 6 && got.chanspec == 0x1006 && got.bssid[5] == 1,
          "BSS fields: '%s' %d ch %u", got.ssid, got.rssi, got.channel);
    CHECK(got.security == SEC_WPA2 && got.group_cipher == 4 && got.pairwise == 4 && got.mfp == MFP_CAPABLE, "WPA2-PSK CCMP, MFP capable (sec %x)", got.security);

    static const uint8_t rsn_sae[] = { 48, 24, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 8, 0x80, 0 };
    n = mk_escan(d, "Mixed", rsn_sae, sizeof rsn_sae, 0x0411, -70);
    fm_escan_parse(d, n, take, 0);
    CHECK(got.security == (SEC_WPA2 | SEC_WPA3), "WPA2/WPA3 transition (sec %x)", got.security);

    static const uint8_t wpa1[] = { 221, 22, 0, 0x50, 0xf2, 1, 1, 0, 0, 0x50, 0xf2, 2, 1, 0, 0, 0x50, 0xf2, 2, 1, 0, 0, 0x50, 0xf2, 2 };
    n = mk_escan(d, "Old", wpa1, sizeof wpa1, 0x0411, -80);
    fm_escan_parse(d, n, take, 0);
    CHECK(got.security == SEC_WPA && got.group_cipher == 2 && got.pairwise == 2, "WPA1 TKIP (sec %x)", got.security);

    n = mk_escan(d, "Cafe", (const uint8_t *)"", 0, 0x0401, -60);
    fm_escan_parse(d, n, take, 0);
    CHECK(got.security == SEC_OPEN, "open");
    n = mk_escan(d, "Wep", (const uint8_t *)"", 0, 0x0411, -60);
    fm_escan_parse(d, n, take, 0);
    CHECK(got.security == SEC_WEP, "WEP from the privacy bit");
    static const uint8_t ent[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 1, 0, 0 };
    n = mk_escan(d, "Corp", ent, sizeof ent, 0x0411, -60);
    fm_escan_parse(d, n, take, 0);
    CHECK(got.security == SEC_8021X, "enterprise (sec %x)", got.security);
    fm_put16(d + 10, 2);
    CHECK(fm_escan_parse(d, n, take, 0) < 0, "count beyond the buffer refused");
    fm_put16(d + 10, 0);
    CHECK(fm_escan_parse(d, 12, take, 0) == 0, "scan done, no BSS");
}

static void t_join(void)
{
    struct fm_bss b;
    memset(&b, 0, sizeof b);
    b.security = SEC_WPA2; b.group_cipher = 2; b.pairwise = 4;
    uint8_t ie[40];
    static const uint8_t want[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
    unsigned n = fm_build_wpaie(ie, &b, 0);
    CHECK(n == sizeof want && !memcmp(ie, want, n), "RSN element for WPA2-PSK, TKIP group");
    n = fm_build_wpaie(ie, &b, 1);
    CHECK(n == 22 && ie[19] == 8 && ie[20] == 0xc0, "RSN element for SAE, MFP required");
    b.security = SEC_WPA; b.group_cipher = 2; b.pairwise = 2;
    n = fm_build_wpaie(ie, &b, 0);
    CHECK(n == 24 && ie[0] == 221 && ie[1] == 22 && ie[5] == 1 && ie[23] == 2, "WPA vendor element");

    uint8_t j[80];
    const uint8_t bssid[6] = { 1, 2, 3, 4, 5, 6 };
    n = fm_join_params(j, "Home", 4, bssid, 0x1006);
    CHECK(n == 72 && fm_le32(j) == 4 && !memcmp(j + 4, "Home", 4) && j[36] == 0xff && fm_le32(j + 44) == 320 &&
          !memcmp(j + 56, bssid, 6) && fm_le32(j + 64) == 1 && fm_le16(j + 68) == 0x1006, "ext join params");
    n = fm_join_params(j, "Home", 4, 0, 0);
    CHECK(j[56] == 0xff && fm_le32(j + 64) == 0 && fm_le32(j + 40) == 0xffffffffu, "ext join, any BSS");
    n = fm_ssid_params(j, "Home", 4, bssid);
    CHECK(n == 52 && !memcmp(j + 36, bssid, 6) && fm_le32(j + 44) == 0, "SET_SSID params");

    uint8_t k[140];
    n = fm_pmk(k, "secret123", 9, 0);
    CHECK(n == 132 && fm_le16(k) == 9 && fm_le16(k + 2) == 1 && !memcmp(k + 4, "secret123", 9) && k[13] == 0, "PMK");
    CHECK(fm_pmk(k, "secret123", 9, 1) == 70, "short PMK");
    CHECK(fm_sae_password(k, "pw", 2) == 130 && fm_le16(k) == 2 && k[2] == 'p', "SAE password");
    CHECK(fm_country(k, "GB") == 12 && !memcmp(k, "GB\0\0\0\0\0\0GB\0\0", 12), "country");
}

static void t_nvram(void)
{
    static const char txt[] = "# comment\r\nboardflags=0x00404001\r\n  \r\nmanfid=0x2d0\n#x=1\nboardtype=0x6e4";
    uint8_t out[sizeof txt + 16];
    unsigned n = fm_nvram((const uint8_t *)txt, sizeof txt - 1, out);
    static const char want[] = "boardflags=0x00404001\0manfid=0x2d0\0boardtype=0x6e4\0";
    CHECK(!memcmp(out, want, sizeof want), "entries");
    CHECK(n % 4 == 0, "length %u a multiple of 4", n);
    uint32_t tok = fm_le32(out + n - 4), words = (n - 4) / 4;
    CHECK((tok & 0xffff) == words && (tok >> 16) == (~words & 0xffff), "length token %08x for %u words", tok, words);
    CHECK(out[sizeof want - 1] == 0 && n == sizeof want + 4, "list end, then the token (%u)", n);
}

static void t_names(void)
{
    CHECK(!strcmp(fm_fw_stem(0x4345, 6, 0), "cyfmac43455-sdio") && !strcmp(fm_fw_stem(0x4345, 6, 1), "brcmfmac43455-sdio"), "Pi 4 names");
    CHECK(!strcmp(fm_fw_stem(0x4345, 9, 0), "brcmfmac43456-sdio"), "CM4 name");
    CHECK(fm_fw_stem(0x1234, 0, 0) == 0, "unknown chip");
}

int main(void)
{
    t_sdpcm(); t_bcdc(); t_event(); t_scan(); t_join(); t_nvram(); t_names();
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails != 0;
}
