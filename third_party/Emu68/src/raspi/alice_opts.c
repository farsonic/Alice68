/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68's cmdline.txt options - see alice_opts.h. */
#include "alice_opts.h"

const char *const opt_sections[OPT_NSEC] = { "Display", "Storage", "Network", "USB" };

static const char *const n_look[] = { "Sharp", "Smooth", "TV", 0 };
static const char *const o_look[] = { "aga.look=0", "aga.look=1", "aga.look=2" };
static const char *const n_scale[] = { "4:3", "Integer", "Fill the screen", "5:4", "16:9", "16:10", "21:9", "32:9", 0 };
static const char *const o_scale[] = { "aga.scale=0", "aga.scale=1", "aga.scale=2", "aga.scale=3", "aga.scale=4",
                                       "aga.scale=5", "aga.scale=6", "aga.scale=7" };
static const char *const n_shown[] = { "Shown", "Hidden", 0 };
static const char *const o_bootlog[] = { "", "aga.nobootlog" };
static const char *const n_hdf[] = { "Mounted", "Read-only", "Off", 0 };
static const char *const o_hdf[] = { "", "hdf.readonly", "hdf=off" };
static const char *const n_onoff[] = { "On", "Off", 0 };
static const char *const n_mounted[] = { "Mounted", "Off", 0 };
static const char *const o_iso[] = { "", "iso=off" };
static const char *const n_exfat[] = { "As config.txt", "Off", "Read-only", "Read-write", "Read-write, boot", 0 };
static const char *const o_exfat[] = { "", "alice68.exfat=off", "alice68.exfat=ro", "alice68.exfat=rw", "alice68.exfat=boot" };
static const char *const o_wifi[] = { "", "wifi=off" };
static const char *const n_eth[] = { "When opened", "On at boot", "Off", 0 };
static const char *const o_eth[] = { "", "eth=on", "eth=off" };
static const char *const n_mac[] = { "Commodore OUI", "The Pi's own", 0 };
static const char *const o_mac[] = { "", "net.oui=off" };
static const char *const n_usb[] = { "Alice68", "AmigaOS", 0 };
static const char *const o_usb[] = { "", "frf_usb=neptune" };
static const char *const o_modesw[] = { "", "frf_usb.nomodeswitch" };

const struct opt_item opt_items[] = {
    { 0, OPT_CHOICE, 0, 0, 0, 0, "Picture",
      "How AmigaOS's picture is drawn: Sharp keeps every pixel crisp, Smooth filters it, TV adds scanlines and a little blur.",
      "aga.look", n_look, o_look, 0 },
    { 0, OPT_CHOICE, 1, 0, 0, 0, "Scaling",
      "How the picture fills the screen. Integer scales by whole numbers, so pixels stay square and even; the others stretch to an aspect ratio.",
      "aga.scale", n_scale, o_scale, 0 },
    { 0, OPT_CHOICE, 0, 0, 0, 0, "Boot log panels",
      "The SYSTEM and USB panels on this screen while Alice68 starts. Hidden leaves the header and the status bar.",
      "aga.nobootlog", n_shown, o_bootlog, 0 },
    { 1, OPT_CHOICE, 0, 0, 0, 0, "Hardfiles",
      "The .hdf files in the root and the HDF folder of the SD card's FAT or exFAT partition. Every one is mounted; read-only keeps AmigaOS from writing to them.",
      "hdf hdf.readonly hdf.ro", n_hdf, o_hdf, 0 },
    { 1, OPT_TEXTV, 0, OT_UNDERSCORE, 60, 0, "Boot hardfile",
      "The hardfile AmigaOS boots from, by file name (.hdf may be left out). Empty: no hardfile boots, and the SD card's own Amiga partition does.",
      0, 0, 0, "hdf.boot" },
    { 1, OPT_CHOICE, 0, 0, 0, 0, "CD images",
      "The .iso files in the root and the ISO folder, as read-only CD drives CD0:, CD1:, ...",
      "iso", n_mounted, o_iso, 0 },
    { 1, OPT_CHOICE, 0, 0, 0, 0, "exFAT partition",
      "The card's exFAT partition as an Amiga volume. As config.txt: its alice68.exfat= line decides. Read-write lets AmigaOS change it; a volume left half-written is checked and repaired at the next boot. Boot from it makes it the boot drive.",
      "alice68.exfat", n_exfat, o_exfat, 0 },
    { 2, OPT_CHOICE, 0, 0, 0, 0, "WiFi",
      "The Pi's WiFi chip, for alice68net.device unit 0. Off leaves it powered down.",
      "wifi", n_onoff, o_wifi, 0 },
    { 2, OPT_TEXTV, 0, OT_QUOTE, 32, 0, "Network name",
      "The WiFi network joined at boot (its SSID). Empty: join from AmigaOS with Alice68-WiFi or Alice68-Network instead.",
      0, 0, 0, "wifi.ssid" },
    { 2, OPT_TEXTV, 0, OT_QUOTE, 63, 1, "Password",
      "The WiFi network's password (WPA2 or WPA3). Stored in cmdline.txt as it is typed.",
      0, 0, 0, "wifi.psk" },
    { 2, OPT_TEXTV, 0, OT_UPPER, 2, 0, "Country",
      "Two letters, such as GB, DE or US: the WiFi channels allowed where you are.",
      0, 0, 0, "wifi.country" },
    { 2, OPT_CHOICE, 0, 0, 0, 0, "Ethernet",
      "The Pi's Ethernet port, for alice68net.device unit 1: started when a program opens the unit, at boot, or never (Off leaves it to rondoval's genet.device).",
      "eth", n_eth, o_eth, 0 },
    { 2, OPT_CHOICE, 0, 0, 0, 0, "MAC addresses",
      "Both network units normally use Commodore's OUI, 00:80:10, with the rest of the Pi's own address. The Pi's own keeps its addresses as they are.",
      "net.oui", n_mac, o_mac, 0 },
    { 3, OPT_CHOICE, 0, 0, 0, 0, "USB-A ports",
      "Alice68 drives the four USB-A ports itself: keyboard, mouse and Neptune's devices. AmigaOS leaves them to rondoval's xhci.device (frf_usb=neptune); keyboard and mouse then come from the USB-C port.",
      "frf_usb", n_usb, o_usb, 0 },
    { 3, OPT_CHOICE, 0, 0, 0, 0, "Dongle mode switch",
      "Realtek USB network adapters start as a driver CD. Alice68 switches them to network mode; Off leaves them as they come.",
      "frf_usb.nomodeswitch", n_onoff, o_modesw, 0 },
};
const int opt_count = (int)(sizeof opt_items / sizeof *opt_items);

/* ---- small helpers (no C library) ---------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void scpy(char *d, const char *s, unsigned max) { unsigned i = 0; for (; s && s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* the next option of a command line: its start and length (quotes keep spaces in) */
static const char *next_tok(const char *c, unsigned *len)
{
    while (*c && is_sp(*c)) c++;
    const char *t = c;
    int q = 0;
    while (*c && (q || !is_sp(*c))) { if (*c == '"') q = !q; c++; }
    *len = (unsigned)(c - t);
    return *len ? t : 0;
}
static int tok_is(const char *t, unsigned n, const char *w) { unsigned k = slen(w); if (k != n) return 0; for (unsigned i = 0; i < n; i++) if (t[i] != w[i]) return 0; return 1; }
/* "key=..." with this key */
static int tok_key(const char *t, unsigned n, const char *key) { unsigned k = slen(key); if (n <= k || t[k] != '=') return 0; for (unsigned i = 0; i < k; i++) if (t[i] != key[i]) return 0; return 1; }

int opt_nchoices(const struct opt_item *it) { int n = 0; while (it->names[n]) n++; return n; }

void opt_init(struct opt_vals *s, const char *b)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
    for (int i = 0; i < opt_count; i++) {
        const struct opt_item *it = &opt_items[i];
        s->cur[i] = it->def;
        unsigned n;
        for (const char *c = b ? b : "", *t; (t = next_tok(c, &n)); c = t + n) {
            if (it->kind == OPT_CHOICE) {
                for (int k = 0; k < opt_nchoices(it); k++) if (it->opts[k][0] && tok_is(t, n, it->opts[k])) s->cur[i] = (uint8_t)k;
            } else if (tok_key(t, n, it->key)) {
                const char *v = t + slen(it->key) + 1;
                unsigned vn = n - slen(it->key) - 1, k = 0;
                if (vn >= 2 && v[0] == '"' && v[vn - 1] == '"') { v++; vn -= 2; }
                for (; k < vn && k + 1 < OPT_TEXT; k++) s->text[i][k] = (it->flags & OT_UNDERSCORE) && v[k] == '_' ? ' ' : v[k];
                s->text[i][k] = 0;
            }
        }
        s->orig[i] = s->cur[i];
        scpy(s->otext[i], s->text[i], OPT_TEXT);
    }
}

int opt_changed(const struct opt_vals *s)
{
    for (int i = 0; i < opt_count; i++)
        if (opt_items[i].kind == OPT_CHOICE ? s->cur[i] != s->orig[i] : !seq(s->text[i], s->otext[i])) return 1;
    return 0;
}

int opt_edits(const struct opt_vals *s, char *out, unsigned max)
{
    unsigned n = 0;
#define EPUT(ch) do { if (n + 1 >= max) return -1; out[n++] = (char)(ch); } while (0)
#define ESTR(str) do { for (const char *p_ = (str); *p_; p_++) EPUT(*p_); } while (0)
    for (int i = 0; i < opt_count; i++) {
        const struct opt_item *it = &opt_items[i];
        if (it->kind == OPT_CHOICE) {
            if (s->cur[i] == s->orig[i]) continue;
            for (const char *k = it->keys; *k; ) {                 /* -name for each option it owns */
                EPUT('-');
                while (*k && *k != ' ') { EPUT(*k); k++; }
                EPUT('\n');
                while (*k == ' ') k++;
            }
            if (it->opts[s->cur[i]][0]) { EPUT('+'); ESTR(it->opts[s->cur[i]]); EPUT('\n'); }
        } else {
            if (seq(s->text[i], s->otext[i])) continue;
            EPUT('-'); ESTR(it->key); EPUT('\n');
            if (s->text[i][0]) {
                const char *v = s->text[i];
                int sp = 0;
                for (const char *p = v; *p; p++) if (*p == ' ') sp = 1;
                EPUT('+'); ESTR(it->key); EPUT('=');
                int q = (it->flags & OT_QUOTE) && sp;
                if (q) EPUT('"');
                for (const char *p = v; *p; p++) EPUT((it->flags & OT_UNDERSCORE) && *p == ' ' ? '_' : *p);
                if (q) EPUT('"');
                EPUT('\n');
            }
        }
    }
    out[n] = 0;
    return (int)n;
#undef EPUT
#undef ESTR
}

int opt_item_changed(const struct opt_vals *s, int i)
{
    return opt_items[i].kind == OPT_CHOICE ? s->cur[i] != s->orig[i] : !seq(s->text[i], s->otext[i]);
}

void opt_written(const struct opt_vals *s, int i, char *out, unsigned max)
{
    const struct opt_item *it = &opt_items[i];
    unsigned k = 0;
    out[0] = 0;
    if (!max) return;
    if (it->kind == OPT_CHOICE) { scpy(out, it->opts[s->cur[i]], max); return; }
    if (!s->text[i][0]) return;
    int sp = 0;
    for (const char *p = s->text[i]; *p; p++) if (*p == ' ') sp = 1;
    int q = (it->flags & OT_QUOTE) && sp;
    for (const char *p = it->key; *p && k + 1 < max; ) out[k++] = *p++;
    if (k + 1 < max) out[k++] = '=';
    if (q && k + 1 < max) out[k++] = '"';
    for (const char *p = s->text[i]; *p && k + 1 < max; p++) out[k++] = (it->flags & OT_UNDERSCORE) && *p == ' ' ? '_' : *p;
    if (q && k + 1 < max) out[k++] = '"';
    out[k] = 0;
}
