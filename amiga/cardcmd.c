/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 CARD and Alice68 BT - the SD card and the kernel's Bluetooth from the Shell: what
 * Alice68Prefs's Card, Files, Settings, Text files and Bluetooth pages do, through the same
 * code (cardcore.c).
 *
 *   Alice68 CARD [INFO] [FREE]               partitions, free space (FREE: also on FAT), what is mounted
 *   Alice68 CARD LIST [path] [EXFAT]         a folder of the FAT (or exFAT) partition
 *   Alice68 CARD DELETE path [EXFAT]         a file, or an empty folder
 *   Alice68 CARD RENAME path newpath [EXFAT]
 *   Alice68 CARD MKDIR path [EXFAT]
 *   Alice68 CARD OPTIONS                     cmdline.txt's options as the Settings page has them
 *   Alice68 CARD SET option ...              KEY=VALUE, an option as written (aga.scale=4) or LABEL=NAME
 *   Alice68 CARD BOOT name.hdf               the hardfile to boot from (hdf.boot=)
 *   Alice68 CARD KICKSTART name.rom          config.txt's initramfs line
 *   Alice68 CARD CONFIG                      config.txt line by line, and cmdline.txt (password hidden)
 *   Alice68 CARD CONFIG SET n text | ADD text | INSERT n text | DELETE n   a line of config.txt
 *   Alice68 CARD CMDLINE ["line"]            cmdline.txt as one line; typed with wifi.psk=******** the
 *                                            password stays as it is
 *   Alice68 CARD RESTART                     restart the Pi (through the network board)
 *   Alice68 BT [LIST]                        state, paired and found devices
 *   Alice68 BT SCAN | PAIR addr | CONNECT addr | FORGET addr | KEEP
 *
 * SET, BOOT, KICKSTART, CMDLINE and CONFIG's changes write cmdline.txt or config.txt at once, the old one kept as .bak.
 * KEEP writes the paired list into cmdline.txt (bt.dev=), as Alice68Prefs does by itself.
 */
#include "wifi/wifios.h"
#include "cardcore.h"
#include "../third_party/Emu68/src/alice/alice_net.h"

static APTR dos; static ULONG out;
static void put(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void dec(ULONG v) { char b[12]; int n = 11; b[n] = 0; do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v); put(b + n); }
static void sdec(char *t, ULONG v) { char r[12]; int m = 0, k = 0; do { r[m++] = (char)('0' + v % 10); v /= 10; } while (v); while (m) t[k++] = r[--m]; t[k] = 0; }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void pad(const char *s, unsigned w) { put(s); for (unsigned n = slen(s); n < w; n++) put(" "); }
static char uc(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static int is(const char *a, const char *w) { while (*a && uc(*a) == *w) { a++; w++; } return !*a && !*w; }

/* the words of the line; quotes keep spaces */
#define NW 16
static char wbuf[512];
static char *word[NW];
static int nw;
static void split(const char *a, ULONG n)
{
    ULONG i = 0, k = 0;
    nw = 0;
    while (nw < NW) {
        while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n')) i++;
        if (i >= n) break;
        word[nw++] = wbuf + k;
        int q = a[i] == '"';
        if (q) i++;
        while (i < n && k < sizeof wbuf - 2 && (q ? a[i] != '"' : a[i] != ' ' && a[i] != '\t' && a[i] != '\n')) wbuf[k++] = a[i++];
        if (q && i < n) i++;
        wbuf[k++] = 0;
    }
}
/* EXFAT anywhere: the exFAT partition; it is taken out of the words */
static unsigned vol_word(void)
{
    for (int i = 0; i < nw; i++)
        if (is(word[i], "EXFAT")) { for (int k = i; k + 1 < nw; k++) word[k] = word[k + 1]; nw--; return CS_EXFAT; }
    return CS_FAT;
}
static int fail(const char *what, int e) { put(what); put(": "); put(cc_err(e)); put("\n"); return 10; }

static void kb(ULONG v) { if (v >= 10u << 20) { dec(v >> 20); put(" GB"); } else if (v >= 10u << 10) { dec(v >> 10); put(" MB"); } else { dec(v); put(" KB"); } }
static void bytes(ULONG lo, ULONG hi)
{
    if (hi) { dec(hi * 4 + (lo >> 30)); put(" GB"); }
    else if (lo >= 10u << 20) { dec(lo >> 20); put(" MB"); }
    else if (lo >= 10u << 10) { dec(lo >> 10); put(" KB"); }
    else { dec(lo); put(" bytes"); }
}
static const char *kind_name(ULONG k)
{
    static const char *const n[] = { "empty", "FAT16", "FAT32", "exFAT", "Amiga (RDB)", "extended", "other" };
    return k < 7 ? n[k] : "?";
}

static int info(int count_free)
{
    static struct cs_info i;
    int e = cc_info(&i, count_free);
    if (e) return fail("Card", e);
    put("Card: "); put(i.card_dev); put(i.card_ro ? ", read-only" : ""); put(i.cmd64 ? "" : ", only the first 4 GB"); put("\n");
    put("  #  Kind         Start      Size       Free       Label\n");
    for (ULONG p = 0; p < i.nparts && p < CS_MAXPART; p++) {
        struct cs_part *x = &i.part[p];
        put("  "); dec(p + 1); put(x->boot ? "* " : "  "); pad(kind_name(x->kind), 13);
        char t[16]; sdec(t, x->start); pad(t, 11);
        if (x->total_kb) { kb(x->total_kb); put("  "); } else { dec(x->count >> 11); put(" MB  "); }
        if (x->free_kb != ~0u && x->total_kb) { kb(x->free_kb); put("  "); } else put("-        ");
        put(x->label); put(x->vol == CS_FAT ? "  (Alice68's FAT)" : x->vol == CS_EXFAT ? "  (Alice68's exFAT)" : ""); put("\n");
    }
    put(i.exfat_rw ? "exFAT: written\n" : "exFAT: read-only: "); if (!i.exfat_rw) { put(i.exfat_why); put("\n"); }
    put("In use: "); dec(i.nused); put(i.nused == 1 ? " file\n" : " files\n");
    for (ULONG u = 0; u < i.nused && u < CS_MAXUSED; u++) {
        put("  "); pad(i.used[u].kind == 1 ? "hardfile" : "CD image", 10); pad(i.used[u].vol == CS_EXFAT ? "exFAT" : "FAT", 7);
        put(i.used[u].name); put("\n");
    }
    return 0;
}

static struct cc_ent ents[CC_MAXF];
static int list(unsigned vol, const char *path)
{
    int n, more, e = cc_list(vol, path, ents, CC_MAXF, &n, &more);
    if (e) return fail("List", e);
    put(vol == CS_EXFAT ? "exFAT:" : "FAT:"); put(path[0] ? path : "(root)"); put("\n");
    for (int i = 0; i < n; i++) {
        char nm[112]; unsigned k = 0;
        for (; ents[i].name[k] && k < 100; k++) nm[k] = ents[i].name[k];
        if (ents[i].flags & CSF_DIR) nm[k++] = '/';
        nm[k] = 0;
        put("  "); pad(nm, 40);
        if (ents[i].flags & CSF_DIR) put("folder"); else bytes(ents[i].size, ents[i].sizehi);
        if (ents[i].flags & CSF_INUSE) put("  in use");
        if (cc_is_rom(ents[i].name, ents[i].size) && !(ents[i].flags & CSF_DIR)) put("  Kickstart?");
        put("\n");
    }
    if (more) put("  (more than 256: the first 256 are shown)\n");
    return 0;
}

static void options(void)
{
    char v[OPT_TEXT + 8];
    for (int s = 0; s < OPT_NSEC; s++) {
        put(opt_sections[s]); put("\n");
        for (int i = 0; i < opt_count; i++) {
            if (opt_items[i].sec != s) continue;
            cc_option_shown(i, v, sizeof v);
            put("  "); pad(opt_items[i].label, 22); put(v[0] ? v : "-");
            if (opt_item_changed(cc_opts(), i)) put("   (changed)");
            put("\n");
        }
    }
}
static int save(void)
{
    int f, r = cc_save(&f);
    if (r < 0) return fail(f == 2 ? "config.txt not saved" : "cmdline.txt not saved", -r);
    put(!r ? "Nothing changed\n" : r == 3 ? "Saved cmdline.txt and config.txt (old copies: .bak). Restart the Pi to use them.\n" :
        r == 1 ? "Saved cmdline.txt (old copy: cmdline.txt.bak). Restart the Pi to use it.\n" :
        "Saved config.txt (old copy: config.txt.bak). Restart the Pi to use it.\n");
    return 0;
}

static int card(void)
{
    const char *c = nw ? word[0] : "INFO";
    if (is(c, "RESTART")) {
        if (cc_restart() < 0) { put("No Alice68 network board: restart the Pi by hand (or Ctrl + Left Shift + F11)\n"); return 10; }
        put("Restarting the Pi...\n");
        return 0;
    }
    if (!is(c, "INFO") && !is(c, "LIST") && !is(c, "DELETE") && !is(c, "RENAME") && !is(c, "MKDIR") && !is(c, "OPTIONS") &&
        !is(c, "SET") && !is(c, "BOOT") && !is(c, "KICKSTART") && !is(c, "CONFIG") && !is(c, "CMDLINE")) {
        put("Alice68 CARD [INFO [FREE] | LIST [path] | DELETE path | RENAME path newpath | MKDIR path] [EXFAT]\n"
            "Alice68 CARD OPTIONS | SET option ... | BOOT name.hdf | KICKSTART [name.rom] | RESTART\n"
            "Alice68 CARD CONFIG [SET n text | ADD text | INSERT n text | DELETE n] | CMDLINE [\"line\"]\n");
        return c[0] == '?' ? 0 : 10;
    }
    unsigned vol = vol_word();
    if (is(c, "INFO")) return info(nw > 1 && is(word[1], "FREE"));
    if (is(c, "LIST")) return list(vol, nw > 1 ? word[1] : "");
    if (is(c, "DELETE") || is(c, "MKDIR")) {
        if (nw < 2) { put("which?\n"); return 10; }
        int e = is(c, "DELETE") ? cc_delete(vol, word[1]) : cc_mkdir(vol, word[1]);
        if (e) return fail(word[1], e);
        put(is(c, "DELETE") ? "Deleted " : "Made the folder "); put(word[1]); put("\n");
        return 0;
    }
    if (is(c, "RENAME")) {
        if (nw < 3) { put("RENAME path newpath\n"); return 10; }
        int e = cc_rename(vol, word[1], word[2]);
        if (e) return fail(word[1], e);
        put("Renamed to "); put(word[2]); put("\n");
        return 0;
    }
    int e = cc_load();
    if (e) return fail("cmdline.txt and config.txt", e);
    if (is(c, "OPTIONS")) { options(); return 0; }
    if (is(c, "CONFIG") && nw > 1) {                      /* CONFIG SET n text | ADD text | INSERT n text | DELETE n */
        ULONG v = 0; const char *q = nw > 2 ? word[2] : "";
        for (const char *p = q; *p >= '0' && *p <= '9'; p++) v = v * 10 + (ULONG)(*p - '0');
        int r = is(word[1], "ADD") ? cc_line_set(cc_nlines(), q) :
                is(word[1], "SET") && nw > 3 && v ? cc_line_set((int)v - 1, word[3]) :
                is(word[1], "INSERT") && nw > 3 && v ? cc_line_insert((int)v - 1, word[3]) :
                is(word[1], "DELETE") && v ? cc_line_delete((int)v - 1) : -2;
        if (r == -2) { put("CONFIG [SET n text | ADD text | INSERT n text | DELETE n]  (n: the line's number in CARD CONFIG)\n"); return 10; }
        if (r) { put("no such line, or too many lines\n"); return 10; }
        return save();
    }
    if (is(c, "CMDLINE")) {                                    /* the whole line; wifi.psk=******** keeps the password */
        if (nw < 2) { static char line[1200]; cc_cmdline_shown(line, sizeof line); put(line); put("\n"); return 0; }
        cc_cmdline_typed(word[1]);
        return save();
    }
    if (is(c, "CONFIG")) {
        put("config.txt\n");
        for (int i = 0; i < cc_nlines(); i++) { char t[12]; sdec(t, (ULONG)i + 1); put("  "); pad(t, 4); put(cc_line(i)); put("\n"); }
        static char line[1200];
        cc_cmdline_shown(line, sizeof line);
        put("cmdline.txt\n  "); put(line); put("\n");
        return 0;
    }
    if (is(c, "SET")) {
        if (nw < 2) { put("SET option ...  (KEY=VALUE, an option as written, or LABEL=NAME; see CARD OPTIONS)\n"); return 10; }
        for (int i = 1; i < nw; i++)
            if (cc_set_option(word[i])) { put("no such option: "); put(word[i]); put("\n"); return 10; }
        return save();
    }
    if (is(c, "BOOT")) {
        if (nw < 2 || !cc_ends(word[1], ".hdf")) { put("BOOT name.hdf\n"); return 10; }
        cc_set_boot(word[1]);
        return save();
    }
    /* KICKSTART */
    if (nw < 2) {
        put("Kickstart ROMs on the card:\n");
        for (int i = 0; i < cc_nroms(); i++) { put(i == cc_rom_cur() ? "* " : "  "); put(cc_rom(i)); put("\n"); }
        return 0;
    }
    if (cc_set_kickstart(word[1])) { put("config.txt has too many lines\n"); return 10; }
    return save();
}

static int bt(void)
{
    volatile unsigned char *b = cc_board();
    if (!b) { put("No Alice68 network board: Bluetooth needs an Alice68 kernel\n"); return 10; }
    volatile struct an_bt *x = (volatile struct an_bt *)(b + AN_OFF_BT);
    const char *c = nw ? word[0] : "LIST";
    char ad[20];
    if (is(c, "LIST")) {
        char m[65]; int i = 0;
        for (; i < 64 && x->msg[i]; i++) m[i] = x->msg[i];
        m[i] = 0;
        put("Bluetooth: "); put(cc_bt_state(x->state)); if (m[0] && x->state != AN_BT_NOFW) { put(": "); put(m); } put("\n");
        if (x->passkey_shown) { put("Type "); char d[7]; ULONG v = x->passkey; for (int k = 5; k >= 0; k--) { d[k] = (char)('0' + v % 10); v /= 10; } d[6] = 0; put(d); put(" on the keyboard, then Enter\n"); }
        if (x->scanning) put("Scanning...\n");
        put("Paired:\n  Name                          Kind            Radio    Status         Address\n");
        for (ULONG k = 0; k < x->nkeys && k < AN_BT_KEYS; k++) {
            volatile struct an_btkey *p = &x->keys[k];
            char nm[41]; int j = 0; for (; j < 40 && p->name[j]; j++) nm[j] = p->name[j]; nm[j] = 0;
            cc_bt_addr(ad, p->addr);
            put("  "); pad(nm[0] ? nm : "(no name)", 30); pad(cc_bt_kind(p->flags), 16); pad(p->flags & AN_BTK_LE ? "LE" : "Classic", 9);
            pad(p->flags & AN_BTK_CONNECTED ? "connected" : "not connected", 15); put(ad); put("\n");
        }
        put("Found:\n  Name                          Kind            Radio    Signal         Address\n");
        for (ULONG k = 0; k < x->nfound && k < AN_BT_FOUND; k++) {
            volatile struct an_btfound *f = &x->found[k];
            char nm[41]; int j = 0; for (; j < 40 && f->name[j]; j++) nm[j] = f->name[j]; nm[j] = 0;
            cc_bt_addr(ad, f->addr);
            put("  "); pad(nm[0] ? nm : "(no name yet)", 30); pad(cc_bt_found_kind(f->cod), 16); pad(f->cod & 0x80000000u ? "LE" : "Classic", 9);
            char sg[16] = "-";
            if (f->rssi < 0) { sg[0] = '-'; sdec(sg + 1, (ULONG)-f->rssi); unsigned q = slen(sg); sg[q] = ' '; sg[q + 1] = 'd'; sg[q + 2] = 'B'; sg[q + 3] = 'm'; sg[q + 4] = 0; }
            pad(sg, 15);
            put(ad); put("\n");
        }
        return 0;
    }
    if (is(c, "KEEP")) {
        int e = cc_load();
        if (!e) e = cc_bt_keep();
        if (e) return fail("The paired devices could not be kept in cmdline.txt", e < 0 ? CSE_TOOBIG : e);
        put("The paired devices are kept in cmdline.txt (bt.dev=)\n");
        return 0;
    }
    unsigned cmd = is(c, "SCAN") ? AN_BT_CMD_SCAN : is(c, "PAIR") ? AN_BT_CMD_PAIR : is(c, "CONNECT") ? AN_BT_CMD_CONNECT :
                   is(c, "FORGET") ? AN_BT_CMD_FORGET : 0;
    if (!cmd) { put("Alice68 BT [LIST | SCAN | PAIR addr | CONNECT addr | FORGET addr | KEEP]\n"); return c[0] == '?' ? 0 : 10; }
    unsigned char a[6] = { 0 };
    if (cmd != AN_BT_CMD_SCAN && (nw < 2 || cc_bt_parse_addr(word[1], a))) { put("an address as AA:BB:CC:DD:EE:FF (see BT LIST)\n"); return 10; }
    int r = cc_bt_cmd(cmd, a);
    if (r == -2) { put("Bluetooth is not ready: "); put(cc_bt_state(x->state)); put("\n"); return 10; }
    put(cmd == AN_BT_CMD_SCAN ? "Scanning for 10 seconds: put the device in pairing mode now (BT LIST shows what is found)\n" :
        cmd == AN_BT_CMD_PAIR ? "Pairing (BT LIST shows how it goes; then BT KEEP, or Alice68Prefs keeps it)\n" :
        cmd == AN_BT_CMD_CONNECT ? "Connecting\n" : "Forgotten (BT KEEP writes the list into cmdline.txt)\n");
    return 0;
}

static LONG run(const char *a, ULONG n, int which)
{
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);
    split(a, n);
    int e = cc_open();
    LONG rc;
    if (e && which == 0) rc = fail("alice68hdf.device's card service", e < 0 ? CSE_IO : e);
    else rc = which ? bt() : card();
    cc_close();
    CloseLibrary(dos);
    return rc;
}
LONG card_main(const char *a, ULONG n) { return run(a, n, 0); }
LONG bt_main(const char *a, ULONG n) { return run(a, n, 1); }
