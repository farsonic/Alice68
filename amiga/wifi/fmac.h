/* SPDX-License-Identifier: GPL-2.0-only
 * alice68wifi.device: the Broadcom/Cypress FullMAC protocol, host-independent.
 *
 * Everything here builds and runs on the PC as well as on the 68k: no AmigaOS
 * calls, no structs laid over wire data (the 68k gcc aligns 32-bit fields to
 * 2 bytes, the firmware to 4), every field read and written byte by byte at
 * the offset the firmware uses. tools/wifi-test checks it on the PC.
 *
 * Written from Linux's brcmfmac and OpenBSD's bwfm (both ISC) and the
 * IEEE 802.11 element formats. */
#ifndef ALICE68_FMAC_H
#define ALICE68_FMAC_H

#include <stdint.h>
#include <stddef.h>

/* ---- little/big-endian fields ------------------------------------------- */
static inline uint16_t fm_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t fm_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t fm_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t fm_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void fm_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void fm_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---- SDPCM: the SDIO framing (4-byte hardware + 8-byte software header) -- */
#define SDPCM_HDRLEN        12
#define SDPCM_CHAN_CONTROL  0
#define SDPCM_CHAN_EVENT    1
#define SDPCM_CHAN_DATA     2
#define SDPCM_CHAN_GLOM     3
#define F2_BLOCK            512

struct sdpcm_rx {
    unsigned len;       /* whole frame, headers included */
    unsigned chan;
    unsigned dataoff;   /* payload offset from the frame start */
    unsigned nextlen;   /* read-ahead hint for the next frame, bytes (0 = none) */
    uint8_t  seq, maxseq;
    uint8_t  flowctl;
};
/* 0 = frame header OK, 1 = empty (no frame waiting), <0 = corrupt */
int sdpcm_parse(const uint8_t *hdr, struct sdpcm_rx *r);
/* Write the headers in front of a payload already at buf + SDPCM_HDRLEN (+4 for
 * data frames: the BCDC header, written here too). Returns the length to send,
 * padded with zeros: to 4 bytes, or to whole F2 blocks past one block. */
unsigned sdpcm_frame(uint8_t *buf, unsigned chan, uint8_t seq, unsigned payload);
/* bytes to transfer for a frame of n bytes (the padding rule above) */
static inline unsigned sdpcm_padded(unsigned n) { return n > F2_BLOCK && (n % F2_BLOCK) ? (n + F2_BLOCK - 1) / F2_BLOCK * F2_BLOCK : (n + 3) & ~3u; }
/* credit: may a frame with sequence number seq be sent now? */
static inline int sdpcm_credit(uint8_t seq, uint8_t maxseq) { uint8_t d = (uint8_t)(maxseq - seq); return d != 0 && !(d & 0x80); }

/* ---- BCDC: control messages (ioctls) and the data header ---------------- */
#define BCDC_DCMD_HDR       16
#define BCDC_DATA_HDR       4
#define BCDC_PROTO_VER      2
#define BCDC_SET            0x02
#define BCDC_ERROR          0x01

/* Build an ioctl in buf (payload copied after the 16-byte header unless it is
 * already there). Returns its length. */
unsigned bcdc_dcmd(uint8_t *buf, uint32_t cmd, int set, uint16_t reqid, const void *payload, unsigned len);
/* Name + value of an iovar ("name\0" then the value): the GET_VAR/SET_VAR payload. */
unsigned bcdc_iovar(uint8_t *buf, unsigned max, const char *name, const void *val, unsigned len);
struct bcdc_reply { uint32_t cmd; uint16_t reqid; int error; int32_t status; const uint8_t *data; unsigned len; };
int bcdc_parse_reply(const uint8_t *msg, unsigned len, struct bcdc_reply *r);
/* data frame: the payload after the BCDC header (data_offset in 4-byte words), or NULL */
const uint8_t *bcdc_data(const uint8_t *p, unsigned len, unsigned *outlen);

/* firmware commands */
#define WLC_GET_VERSION     1
#define WLC_UP              2
#define WLC_DOWN            3
#define WLC_GET_RATE        12
#define WLC_SET_INFRA       20
#define WLC_SET_AUTH        22
#define WLC_GET_BSSID       23
#define WLC_SET_SSID        26
#define WLC_GET_CHANNEL     29
#define WLC_DISASSOC        52
#define WLC_SET_PM          86
#define WLC_SET_AP          118
#define WLC_GET_RSSI        127
#define WLC_SET_SCAN_CHANNEL_TIME   185
#define WLC_SET_SCAN_UNASSOC_TIME   187
#define WLC_SET_SCAN_PASSIVE_TIME   258
#define WLC_GET_VAR         262
#define WLC_SET_VAR         263
#define WLC_SET_WSEC_PMK    268

/* wpa_auth / wsec / mfp values */
#define WPA_AUTH_DISABLED   0x0000
#define WPA_AUTH_PSK        0x0004
#define WPA2_AUTH_PSK       0x0080
#define WPA2_AUTH_PSK_SHA256 0x8000
#define WPA3_AUTH_SAE_PSK   0x40000
#define WSEC_NONE           0
#define WSEC_TKIP           0x0002
#define WSEC_AES            0x0004
#define MFP_NONE            0
#define MFP_CAPABLE         1
#define MFP_REQUIRED        2

/* ---- firmware events ------------------------------------------------------ */
#define E_SET_SSID          0
#define E_JOIN              1
#define E_AUTH              3
#define E_DEAUTH            5
#define E_DEAUTH_IND        6
#define E_ASSOC             7
#define E_REASSOC           9
#define E_DISASSOC          11
#define E_DISASSOC_IND      12
#define E_LINK              16
#define E_ROAM              19
#define E_BCNLOST           31
#define E_PSK_SUP           46
#define E_IF                54
#define E_ESCAN_RESULT      69
#define E_MAX               192           /* bits in the event mask */
#define EVENT_MASK_LEN      (E_MAX / 8)
#define E_STATUS_SUCCESS    0
#define E_STATUS_PARTIAL    8
#define E_STATUS_FWSUP_COMPLETED 6
#define E_FLAG_LINK         0x01

struct fm_event { uint32_t type, status, reason, flags; uint8_t addr[6]; const uint8_t *data; unsigned datalen; };
/* A frame from the data or event channel (after the BCDC header): 1 if it is a
 * firmware event (filled in), 0 if it is an ordinary Ethernet frame. */
int fm_event_parse(const uint8_t *frame, unsigned len, struct fm_event *e);
static inline void fm_event_mask_set(uint8_t *mask, unsigned ev) { mask[ev / 8] |= (uint8_t)(1u << (ev % 8)); }

/* ---- scanning ------------------------------------------------------------- */
#define SEC_OPEN    0x01
#define SEC_WEP     0x02
#define SEC_WPA     0x04    /* WPA1, vendor IE */
#define SEC_WPA2    0x08    /* RSN with PSK */
#define SEC_WPA3    0x10    /* RSN with SAE */
#define SEC_8021X   0x20    /* enterprise: not supported */

struct fm_bss {
    uint8_t  bssid[6];
    uint8_t  ssid_len;
    char     ssid[33];
    int16_t  rssi;
    uint16_t chanspec;
    uint8_t  channel;
    uint8_t  security;      /* SEC_* bits the network offers */
    uint8_t  group_cipher;  /* RSN/WPA suite type of the group cipher (2 TKIP, 4 CCMP) */
    uint8_t  pairwise;      /* best pairwise suite offered (4 CCMP, else 2 TKIP) */
    uint8_t  mfp;           /* 0 none, 1 capable, 2 required (RSN capabilities) */
};
/* escan parameters for "escan" (start a scan of all channels); returns length */
unsigned fm_escan_params(uint8_t *buf, uint16_t sync_id);
/* One E_ESCAN_RESULT payload: calls fn for each BSS in it. Returns the BSS count, <0 if corrupt. */
int fm_escan_parse(const uint8_t *data, unsigned len, void (*fn)(void *ctx, const struct fm_bss *b), void *ctx);
/* the information elements of one bss_info record -> security fields of b */
void fm_parse_ies(const uint8_t *ie, unsigned len, struct fm_bss *b);

/* ---- joining -------------------------------------------------------------- */
/* The RSN (or WPA vendor) element the firmware puts in the association request,
 * for PSK (akm 2) or SAE (akm 8) with the AP's ciphers. Returns its length. */
unsigned fm_build_wpaie(uint8_t *buf, const struct fm_bss *b, int sae);
/* "join" iovar value (extended join params), with bssid/chanspec or broadcast. Returns its length (72). */
unsigned fm_join_params(uint8_t *buf, const char *ssid, unsigned ssid_len, const uint8_t *bssid, uint16_t chanspec);
/* WLC_SET_SSID value (plain join params), the fallback. Returns its length (52). */
unsigned fm_ssid_params(uint8_t *buf, const char *ssid, unsigned ssid_len, const uint8_t *bssid);
/* WLC_SET_WSEC_PMK value: the passphrase for the firmware supplicant. Returns its length. */
unsigned fm_pmk(uint8_t *buf, const char *pass, unsigned len, int short_form);
/* "sae_password" value. Returns its length. */
unsigned fm_sae_password(uint8_t *buf, const char *pass, unsigned len);
/* "country" value for a two-letter ISO code. Returns its length (12). */
unsigned fm_country(uint8_t *buf, const char *cc);

/* ---- NVRAM ---------------------------------------------------------------- */
/* The .txt NVRAM file -> the image the chip wants: comments and line breaks
 * removed, NUL-separated entries, padded to 4, length token at the end.
 * out needs len + 12 bytes. Returns the image length. */
unsigned fm_nvram(const uint8_t *in, unsigned len, uint8_t *out);

/* ---- misc ----------------------------------------------------------------- */
/* d11ac chanspec -> channel number */
static inline unsigned fm_chanspec_channel(uint16_t cs) { return cs & 0xff; }
/* firmware file name stems for a chip id/revision ("cyfmac43455-sdio" etc.), NULL if unknown */
const char *fm_fw_stem(uint16_t chip, uint8_t rev, int alt);

#endif
