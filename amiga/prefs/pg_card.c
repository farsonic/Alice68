/*
 * Alice68Prefs: the SD card's pages - Card (partitions, free space, what is mounted), Files
 * (browse, delete, rename, new folder, boot from a hardfile, the Kickstart), Settings
 * (cmdline.txt's options, the table src/raspi/alice_opts.c shares with the setup screen), Text
 * files (config.txt line by line, cmdline.txt as one line) and Bluetooth (paired and found
 * devices; scan, pair, connect, forget). This was Alice68-Card; what it does is in
 * amiga/cardcore.c, shared with `Alice68 CARD` and `Alice68 BT` (and tested through them).
 *
 * cmdline.txt and config.txt are written only by Save, the old ones kept as .bak; a Bluetooth
 * pairing goes into cmdline.txt (bt.dev=) at once, or it would be lost at the next start. The
 * WiFi password is never shown: "********" stands for it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ui.h"
#include "../cardcore.h"
#include "alice_net.h"

enum {
	ID_FREE = ID_CARD, ID_VOL, ID_FILTER, ID_PARENT, ID_FILES, ID_FOPEN, ID_BOOT, ID_KICK, ID_DELETE, ID_RENAME,
	ID_MKDIR, ID_LINES, ID_LSET, ID_LADD, ID_LINS, ID_LDEL, ID_CMDLINE, ID_OPT,
	ID_BPAIRED = ID_BT, ID_BFOUND, ID_BSCAN, ID_BPAIR, ID_BCONN, ID_BFORGET
};

static int cs_ok;                                    /* the card service opened */
static int loaded;

/* ==================================================================
 * Card
 * ================================================================== */
static struct cs_info info;
static struct vblock card_v;
static struct rows parts, used;
enum { C_DEV, C_EXFAT, C_N };
static const char *const card_names[C_N] = { "Card", "exFAT" };

static void kb(char *b, int n, ULONG v)
{
	if (v >= 10u << 20)
		rt_snprintf(b, n, "%lu GB", (unsigned long)(v >> 20));
	else if (v >= 10u << 10)
		rt_snprintf(b, n, "%lu MB", (unsigned long)(v >> 10));
	else
		rt_snprintf(b, n, "%lu KB", (unsigned long)v);
}

static const char *kind_name(ULONG k)
{
	static const char *const n[] = { "empty", "FAT16", "FAT32", "exFAT", "Amiga (RDB)", "extended", "other" };
	return k < 7 ? n[k] : "?";
}

static void card_show(int count_free)
{
	char b[VLEN], sz[16], fr[16];
	ULONG i;
	int e = cs_ok ? cc_info(&info, count_free) : CSE_NOVOL;

	if (e) {
		vset(&card_v, C_DEV, cs_ok ? cc_err(e) : "alice68hdf.device's card service is not there (not Alice68, or too old)");
		return;
	}
	rt_snprintf(b, sizeof(b), "%s%s%s", info.card_dev, info.card_ro ? ", read-only" : ", read-write",
	            info.cmd64 ? "" : ", only the first 4 GB");
	vset(&card_v, C_DEV, b);
	vset(&card_v, C_EXFAT, info.exfat_rw ? "written (alice68.exfat=rw)" : info.exfat_why);
	rows_begin(&parts);
	for (i = 0; i < info.nparts && i < CS_MAXPART; i++) {
		struct cs_part *x = &info.part[i];
		char *t = rows_add(&parts);
		if (!t)
			break;
		if (x->total_kb)
			kb(sz, sizeof(sz), x->total_kb);
		else
			rt_snprintf(sz, sizeof(sz), "%lu MB", (unsigned long)(x->count >> 11));
		if (x->free_kb != ~0u && x->total_kb)
			kb(fr, sizeof(fr), x->free_kb);
		else
			rt_strlcpy(fr, "-", sizeof(fr));
		rt_snprintf(t, parts.len, "%lu%s\t%s\t%s\t%s\t%s\t%s", (unsigned long)i + 1, x->boot ? " *" : "", kind_name(x->kind),
		            sz, fr, x->label[0] ? x->label : "-",
		            x->vol == CS_FAT ? "Alice68's files" : x->vol == CS_EXFAT ? "Alice68's exFAT" :
		            x->kind == CSP_AMIGA ? "Emu68 RDB disk" : "");
	}
	rows_end(&parts);
	rows_begin(&used);
	for (i = 0; i < info.nused && i < CS_MAXUSED; i++) {
		char *t = rows_add(&used);
		if (!t)
			break;
		rt_snprintf(t, used.len, "%s\t%s\t%s", info.used[i].kind == 1 ? "hardfile" : "CD image",
		            info.used[i].vol == CS_EXFAT ? "exFAT" : "FAT", info.used[i].name);
	}
	rows_end(&used);
}

static Object *card_page_build(void)
{
	Object *g = vgroup(6), *f = button("Count _free space", ID_FREE);

	help(f, "Count free space", "Counts the FAT partition's free space too (reads its whole FAT: a few seconds).");
	parts.obj = table("#\tKind\tSize\tFree\tLabel\tUse", 0, "No partitions.");
	used.obj = table("Mounted\tFrom\tName", 0, "Nothing on the card is mounted.");
	if (!parts.obj || !used.obj)
		return NULL;
	add(g, vblock_build(&card_v, card_names, C_N, 2));
	add(g, parts.obj);
	add(g, row(f, NULL, NULL, NULL));
	add(g, used.obj);
	return page_full(g);
}

/* ==================================================================
 * Files
 * ================================================================== */
static struct cc_ent *fe;                            /* CC_MAXF */
static int nfe, vis[CC_MAXF], nvis;
static struct rows files;
static unsigned fvol = CS_FAT;
static char path[CS_PATH], pathline[CS_PATH + 32];
static Object *f_vol, *f_filter, *f_path, *f_name;
static const char *const vol_names[] = { "FAT partition", "exFAT partition", NULL };
static const char *const filter_names[] = { "All files", "Hardfiles", "CD images", "Kickstart ROMs", "WiFi firmware", NULL };

static int shown_ent(const struct cc_ent *e, int filter)
{
	if (e->flags & CSF_DIR)
		return 1;
	switch (filter) {
	case 1: return cc_ends(e->name, ".hdf");
	case 2: return cc_ends(e->name, ".iso");
	case 3: return cc_is_rom(e->name, e->size);
	case 4: return cc_ends(e->name, ".clm_blob") || !rt_strncasecmp(e->name, "cyfmac", 6) || !rt_strncasecmp(e->name, "brcmfmac", 8);
	}
	return 1;
}

static void files_show(void)
{
	int i, filter = (int)mui_get(f_filter, MUIA_Cycle_Active);

	nvis = 0;
	rows_begin(&files);
	for (i = 0; i < nfe; i++) {
		char sz[24];
		char *t;
		if (!shown_ent(&fe[i], filter) || !(t = rows_add(&files)))
			continue;
		vis[nvis++] = i;
		if (fe[i].flags & CSF_DIR)
			rt_strlcpy(sz, "folder", sizeof(sz));
		else if (fe[i].sizehi)
			rt_snprintf(sz, sizeof(sz), "%lu GB", (unsigned long)(fe[i].sizehi * 4 + (fe[i].size >> 30)));
		else if (fe[i].size >= 10u << 20)
			rt_snprintf(sz, sizeof(sz), "%lu MB", (unsigned long)(fe[i].size >> 20));
		else if (fe[i].size >= 10u << 10)
			rt_snprintf(sz, sizeof(sz), "%lu KB", (unsigned long)(fe[i].size >> 10));
		else
			rt_snprintf(sz, sizeof(sz), "%lu bytes", (unsigned long)fe[i].size);
		rt_snprintf(t, files.len, "%s%s\t%s\t%s", fe[i].name, fe[i].flags & CSF_DIR ? "/" : "", sz,
		            fe[i].flags & CSF_INUSE ? "in use" : !(fe[i].flags & CSF_DIR) && cc_is_rom(fe[i].name, fe[i].size) ? "Kickstart ROM?" : "");
	}
	rows_end(&files);
	mui_set(files.obj, MUIA_Table_Active, (ULONG)-1);
	rt_snprintf(pathline, sizeof(pathline), "%s%s", fvol == CS_EXFAT ? "exFAT:" : "FAT:", path[0] ? path : "(root)");
	mui_set(f_path, MUIA_QLabel_Text, (ULONG)pathline);
}

static void files_load(void)
{
	int more = 0, e = cs_ok ? cc_list(fvol, path, fe, CC_MAXF, &nfe, &more) : CSE_NOVOL;

	if (e) {
		nfe = 0;
		sayf("Files: %s", cc_err(e));
	} else if (more)
		say("Files: this folder has more than 256 entries; the first 256 are shown");
	files_show();
}

static Object *files_page_build(void)
{
	Object *g = vgroup(6), *b[6];

	fe = AllocVec(sizeof(struct cc_ent) * CC_MAXF, MEMF_ANY | MEMF_CLEAR);
	f_vol = cycle(vol_names, ID_VOL);
	f_filter = cycle(filter_names, ID_FILTER);
	f_path = label("FAT:(root)", QL_NOTE);
	f_name = string(100, 0);
	files.obj = table("Name\tSize\t", ID_FILES, "An empty folder.");
	b[0] = button("_Open", ID_FOPEN);
	b[1] = button("_Boot from it", ID_BOOT);
	b[2] = button("Use as _Kickstart", ID_KICK);
	b[3] = button("_Delete...", ID_DELETE);
	b[4] = button("Re_name to", ID_RENAME);
	b[5] = button("New fo_lder", ID_MKDIR);
	if (!fe || !f_vol || !f_filter || !f_path || !f_name || !files.obj || !b[5])
		return NULL;
	help(b[1], "Boot from it", "This hardfile boots from the next start (hdf.boot=). Save, then Restart Pi.");
	help(b[2], "Use as Kickstart", "This ROM (in the FAT partition's root) is the Kickstart from the next start: "
	     "config.txt's initramfs line. Save, then Restart Pi.");
	help(b[3], "Delete", "Deletes the file, or an empty folder, from the SD card (asks first). Mounted hardfiles, "
	     "CD images and open files are refused.");
	help(f_name, "Name", "The new name for Rename, or the name of the new folder.");
	add(g, row(f_vol, f_filter, button("_Parent", ID_PARENT), f_path));
	add(g, files.obj);
	add(g, row(b[0], b[1], b[2], b[3]));
	add(g, row(label("Name", QL_LABEL), f_name, b[4], b[5]));
	return page_full(g);
}

static int fsel(void)
{
	LONG r = (LONG)mui_get(files.obj, MUIA_Table_Active);
	return r >= 0 && r < nvis ? vis[r] : -1;
}

static void files_action(ULONG id)
{
	char p1[CS_PATH], p2[CS_PATH], msg[200];
	const char *name = str_of(f_name);
	int i = fsel(), e;

	if (id == ID_PARENT) {
		cc_parent(path);
		files_load();
		return;
	}
	if (id == ID_MKDIR) {
		if (!name[0]) {
			say("New folder: type its name first");
			return;
		}
		cc_child(p1, path, name);
		e = cc_mkdir(fvol, p1);
		if (e)
			sayf("New folder: %s", cc_err(e));
		else {
			sayf("Made the folder %s", name);
			files_load();
		}
		return;
	}
	if (i < 0) {
		say("Pick a file or folder in the list first");
		return;
	}
	cc_child(p1, path, fe[i].name);
	switch (id) {
	case ID_FOPEN:
		if (!(fe[i].flags & CSF_DIR)) {
			say("That is a file, not a folder");
			return;
		}
		rt_strlcpy(path, p1, sizeof(path));
		files_load();
		break;
	case ID_DELETE:
		rt_snprintf(msg, sizeof(msg), "Delete %s%s\nfrom the SD card? This can't be undone.", fe[i].name,
		            fe[i].flags & CSF_DIR ? " (an empty folder)" : "");
		if (ask(msg, "*_Delete|_Cancel") != 1)
			return;
		e = cc_delete(fvol, p1);
		if (e)
			sayf("Delete: %s", cc_err(e));
		else {
			sayf("Deleted %s", fe[i].name);
			files_load();
		}
		break;
	case ID_RENAME:
		if (!name[0] || !strcmp(name, fe[i].name)) {
			say("Rename: type the new name in Name first");
			return;
		}
		cc_child(p2, path, name);
		e = cc_rename(fvol, p1, p2);
		if (e)
			sayf("Rename: %s", cc_err(e));
		else {
			sayf("Renamed to %s", name);
			files_load();
		}
		break;
	case ID_BOOT:
		if (!cc_ends(fe[i].name, ".hdf")) {
			say("Boot from it: pick a hardfile (.hdf)");
			return;
		}
		cc_set_boot(fe[i].name);
		sayf("%s boots from the next start - Save, then Restart Pi", fe[i].name);
		break;
	case ID_KICK:
		if (fvol != CS_FAT || path[0]) {
			say("Use as Kickstart: the ROM must be in the root of the FAT partition");
			return;
		}
		if (fe[i].flags & CSF_DIR || cc_set_kickstart(fe[i].name)) {
			say("Use as Kickstart: pick a ROM file");
			return;
		}
		sayf("%s is the Kickstart from the next start - Save, then Restart Pi", fe[i].name);
		break;
	}
}

/* ==================================================================
 * Settings: cmdline.txt's options
 * ================================================================== */
static Object *o_gad[OPT_MAX], *o_kick;
static char o_shown[OPT_MAX][OPT_TEXT], kickline[100];

static void opts_show(void)
{
	int i;

	for (i = 0; i < opt_count && i < OPT_MAX; i++) {
		if (!o_gad[i])
			continue;
		if (opt_items[i].kind == OPT_CHOICE)
			mui_nnset(o_gad[i], MUIA_Cycle_Active, cc_opts()->cur[i]);
		else {
			cc_option_shown(i, o_shown[i], OPT_TEXT);
			mui_nnset(o_gad[i], MUIA_String_Contents, (ULONG)o_shown[i]);
		}
	}
	rt_snprintf(kickline, sizeof(kickline), "%s (another: Files, Use as Kickstart)",
	            cc_rom_cur() >= 0 ? cc_rom(cc_rom_cur()) : "none in config.txt");
	if (o_kick)
		mui_set(o_kick, MUIA_QLabel_Text, (ULONG)kickline);
}

/* what is typed into the text options, into the values (the password only when it was changed) */
static void opts_read(void)
{
	int i;

	for (i = 0; i < opt_count && i < OPT_MAX && cs_ok; i++) {
		const char *t;
		if (!o_gad[i])
			continue;
		if (opt_items[i].kind == OPT_CHOICE) {
			cc_opts()->cur[i] = (UBYTE)mui_get(o_gad[i], MUIA_Cycle_Active);
			continue;
		}
		t = str_of(o_gad[i]);
		if (opt_items[i].secret && !strcmp(t, CC_PMASK))
			continue;
		cc_set_text(i, t);
		if (opt_items[i].secret && t[0]) {                       /* typed: hidden again at once */
			rt_strlcpy(o_shown[i], CC_PMASK, OPT_TEXT);
			mui_nnset(o_gad[i], MUIA_String_Contents, (ULONG)o_shown[i]);
		}
	}
}

static Object *settings_page_build(void)
{
	Object *g = vgroup(4);
	int s, i;

	for (s = 0; s < OPT_NSEC; s++) {
		Object *form = mui_new(MUIC_Group, MUIA_Group_Columns, 2, MUIA_Group_HorizSpacing, 8,
		                       MUIA_Group_VertSpacing, theme_compact() ? 1 : 3, TAGEND);
		if (!form)
			return NULL;
		add(g, left(label(opt_sections[s], QL_TITLE)));
		for (i = 0; i < opt_count && i < OPT_MAX; i++) {
			const struct opt_item *it = &opt_items[i];
			Object *l;
			if (it->sec != s)
				continue;
			l = label(it->label, QL_LABEL);
			o_gad[i] = it->kind == OPT_CHOICE ? cycle(it->names, ID_OPT) : string(it->maxlen ? it->maxlen : OPT_TEXT - 1, 0);
			if (!l || !o_gad[i])
				return NULL;
			help(o_gad[i], it->label, it->secret ? "The WiFi network's password. It is never shown: ******** stands for "
			                                       "it. Type a new one to change it." : it->help);
			add(form, l);
			add(form, o_gad[i]);
		}
		add(g, form);
	}
	o_kick = label("", QL_NOTE);
	add(g, row(label("Kickstart", QL_LABEL), o_kick, NULL, NULL));
	add(g, left(label("Save writes cmdline.txt (the old one kept as cmdline.txt.bak); Restart Pi uses it.", QL_NOTE)));
	return page_box(g);
}

/* ==================================================================
 * Text files: config.txt line by line, cmdline.txt as one line
 * ================================================================== */
static struct rows lines;
static Object *t_line, *t_cmd, *t_note;
static char t_cmdtext[1200];

static void lines_show(void)
{
	int i;

	rows_begin(&lines);
	for (i = 0; i < cc_nlines(); i++) {
		char *t = rows_add(&lines);
		if (!t)
			break;
		rt_snprintf(t, lines.len, "%ld\t%s", (long)i + 1, cc_line(i));
	}
	rows_end(&lines);
	cc_cmdline_shown(t_cmdtext, sizeof(t_cmdtext));
	mui_nnset(t_cmd, MUIA_String_Contents, (ULONG)t_cmdtext);
	mui_set(t_note, MUIA_QLabel_Text, (ULONG)(cc_changed() ? "Changes not saved yet: Save writes them (the old files are kept as .bak)"
	                                                      : "As on the card"));
}

static Object *text_files_build(void)
{
	Object *g = vgroup(6), *b[4];

	lines.obj = table("Line\tconfig.txt", ID_LINES, "config.txt is empty.");
	t_line = string(CC_LINE - 1, 0);
	t_cmd = string_id(sizeof(t_cmdtext) - 1, ID_CMDLINE);
	t_note = label("", QL_NOTE);
	b[0] = button("_Change", ID_LSET);
	b[1] = button("_Add", ID_LADD);
	b[2] = button("_Insert", ID_LINS);
	b[3] = button("Dele_te", ID_LDEL);
	if (!lines.obj || !t_line || !t_cmd || !t_note || !b[3])
		return NULL;
	help(t_line, "Line", "Click a line to edit it here; Change puts it back, Add makes it the last line, Insert puts it "
	     "before the one picked.");
	help(t_cmd, "cmdline.txt", "The whole line; Return takes it, and the Settings page follows. wifi.psk=******** keeps "
	     "the password as it is.");
	add(g, lines.obj);
	add(g, row(t_line, NULL, NULL, NULL));
	add(g, row(b[0], b[1], b[2], b[3]));
	add(g, left(label("cmdline.txt", QL_TITLE)));
	add(g, t_cmd);
	add(g, left(t_note));
	return page_full(g);
}

static void lines_action(ULONG id)
{
	LONG r = (LONG)mui_get(lines.obj, MUIA_Table_Active);
	const char *t = str_of(t_line);
	int e = 0;

	switch (id) {
	case ID_LINES:
		if (r >= 0 && r < cc_nlines())
			mui_set(t_line, MUIA_String_Contents, (ULONG)cc_line((int)r));
		return;
	case ID_LSET:
		e = r < 0 ? -1 : cc_line_set((int)r, t);
		break;
	case ID_LADD:
		e = cc_line_set(cc_nlines(), t);
		break;
	case ID_LINS:
		e = cc_line_insert(r < 0 ? 0 : (int)r, t);
		break;
	case ID_LDEL:
		e = r < 0 ? -1 : cc_line_delete((int)r);
		break;
	}
	if (e)
		say(r < 0 && id != ID_LADD ? "Pick a line first" : "config.txt has too many lines");
	else
		say("config.txt changed (Save writes it)");
	lines_show();
	opts_show();
}

/* ==================================================================
 * Bluetooth: the kernel's, through the network board (struct an_bt)
 * ================================================================== */
static struct rows bpaired, bfound, blog;
static Object *b_state, *b_key;
static char b_statetext[120], b_keytext[120];
static ULONG bt_gen_seen = ~0u, bt_sig, blog_seen = ~0u;

static volatile struct an_bt *btb(void)
{
	volatile unsigned char *b = cc_board();
	return b ? (volatile struct an_bt *)(b + AN_OFF_BT) : NULL;
}

static ULONG bt_signature(volatile struct an_bt *x)
{
	ULONG h = x->state * 7 + x->nkeys * 131 + x->nfound * 977 + x->scanning * 3 + x->passkey + x->passkey_shown * 5 + x->keys_gen * 11;
	ULONG i;
	for (i = 0; i < 64 && x->msg[i]; i++)
		h = h * 31 + (UBYTE)x->msg[i];
	for (i = 0; i < x->nkeys && i < AN_BT_KEYS; i++)
		h = h * 17 + x->keys[i].flags;
	for (i = 0; i < x->nfound && i < AN_BT_FOUND; i++)
		h = h * 13 + (UBYTE)x->found[i].name[0] + x->found[i].addr[0] + (ULONG)x->found[i].rssi;
	return h;
}

static void cpy(char *d, const volatile char *s, int n)
{
	int i;
	for (i = 0; i < n - 1 && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
}

static void bt_log_show(void)
{
	volatile unsigned char *nb = cc_board();
	ULONG wr, start, i;
	volatile const char *r;
	char cur[120];
	unsigned n = 0;

	if (!nb || (wr = ((volatile struct an_hdr *)nb)->log_wr) == blog_seen)
		return;
	blog_seen = wr;
	r = (volatile const char *)(nb + AN_OFF_LOG);
	start = wr > AN_LOG_BYTES ? wr - AN_LOG_BYTES : 0;
	rows_begin(&blog);
	for (i = start; i != wr; i++) {
		char c = r[i % AN_LOG_BYTES];
		if (c != '\n') {
			if (n < sizeof(cur) - 1)
				cur[n++] = c;
			continue;
		}
		cur[n] = 0;
		if (n > 4 && !strncmp(cur, "[BT]", 4)) {
			char *t = rows_add(&blog);
			if (!t) {                                       /* full: the oldest goes */
				memmove(blog.buf, blog.buf + blog.len, (ULONG)(blog.max - 1) * (ULONG)blog.len);
				blog.n--;
				t = rows_add(&blog);
			}
			rt_strlcpy(t, cur + 5, blog.len);
		}
		n = 0;
	}
	rows_end(&blog);
	mui_set(blog.obj, MUIA_Table_Top, 100000);
}

static void bt_show(void)
{
	volatile struct an_bt *x = btb();
	char m[65], ad[20];
	ULONG i;

	if (!x) {
		mui_set(b_state, MUIA_QLabel_Text, (ULONG)"No Alice68 network board: Bluetooth needs an Alice68 kernel");
		return;
	}
	cpy(m, x->msg, sizeof(m));
	rt_snprintf(b_statetext, sizeof(b_statetext), "%s%s%s", cc_bt_state(x->state), m[0] && x->state != AN_BT_NOFW ? ": " : "",
	            x->state != AN_BT_NOFW ? m : "");
	mui_set(b_state, MUIA_QLabel_Text, (ULONG)b_statetext);
	if (x->passkey_shown)
		rt_snprintf(b_keytext, sizeof(b_keytext), "Type %06lu on the Bluetooth keyboard, then Enter", (unsigned long)x->passkey);
	else if (x->scanning)
		rt_strlcpy(b_keytext, "Scanning for 10 seconds: put the keyboard, mouse or pad in pairing mode now", sizeof(b_keytext));
	else if (x->pairing)
		rt_strlcpy(b_keytext, "Pairing...", sizeof(b_keytext));
	else
		rt_strlcpy(b_keytext, "Paired devices are kept in cmdline.txt (bt.dev=) and reconnect by themselves.", sizeof(b_keytext));
	mui_set(b_key, MUIA_QLabel_Text, (ULONG)b_keytext);
	rows_begin(&bpaired);
	for (i = 0; i < x->nkeys && i < AN_BT_KEYS; i++) {
		volatile struct an_btkey *k = &x->keys[i];
		char nm[41], *t = rows_add(&bpaired);
		if (!t)
			break;
		cpy(nm, k->name, sizeof(nm));
		cc_bt_addr(ad, k->addr);
		rt_snprintf(t, bpaired.len, "%s\t%s\t%s\t%s\t%s", nm[0] ? nm : "(no name)", cc_bt_kind(k->flags),
		            k->flags & AN_BTK_LE ? "LE" : "Classic", k->flags & AN_BTK_CONNECTED ? "connected" : "not connected", ad);
	}
	rows_end(&bpaired);
	rows_begin(&bfound);
	for (i = 0; i < x->nfound && i < AN_BT_FOUND; i++) {
		volatile struct an_btfound *f = &x->found[i];
		char nm[41], sg[16], *t = rows_add(&bfound);
		if (!t)
			break;
		cpy(nm, f->name, sizeof(nm));
		cc_bt_addr(ad, f->addr);
		if (f->rssi < 0)
			rt_snprintf(sg, sizeof(sg), "%ld dBm", (long)f->rssi);
		else
			rt_strlcpy(sg, "-", sizeof(sg));
		rt_snprintf(t, bfound.len, "%s\t%s\t%s\t%s\t%s", nm[0] ? nm : "(no name yet)", cc_bt_found_kind(f->cod),
		            f->cod & 0x80000000u ? "LE" : "Classic", sg, ad);
	}
	rows_end(&bfound);
	blog_seen = ~0u;
	bt_log_show();
}

static Object *bt_page_build(void)
{
	Object *g = vgroup(4), *b[4];

	b_state = label("", QL_NOTE);
	b_key = label("", QL_NOTE);
	bpaired.obj = table("Paired\tKind\tRadio\tStatus\tAddress", ID_BPAIRED, "Nothing paired yet: Scan, then Pair.");
	bfound.obj = table("Found\tKind\tRadio\tSignal\tAddress", ID_BFOUND, "Nothing found: Scan looks for 10 seconds.");
	blog.obj = table("The Bluetooth log", 0, "Nothing logged.");
	b[0] = button("_Connect now", ID_BCONN);
	b[1] = button("_Forget...", ID_BFORGET);
	b[2] = button("_Scan", ID_BSCAN);
	b[3] = button("_Pair", ID_BPAIR);
	if (!b_state || !b_key || !bpaired.obj || !bfound.obj || !blog.obj || !b[3])
		return NULL;
	help(b[2], "Scan", "Looks for devices in pairing mode for 10 seconds; they appear in the lower list.");
	help(b[3], "Pair", "Pairs the device picked in the lower list. A keyboard may ask for a number: it shows above.");
	add(g, row(label("Bluetooth", QL_LABEL), b_state, NULL, NULL));
	add(g, left(b_key));
	add(g, bpaired.obj);
	add(g, row(b[0], b[1], NULL, NULL));
	add(g, bfound.obj);
	add(g, row(b[2], b[3], NULL, NULL));
	add(g, blog.obj);
	return page_full(g);
}

static void bt_action(ULONG id)
{
	volatile struct an_bt *x = btb();
	LONG pr = (LONG)mui_get(bpaired.obj, MUIA_Table_Active), fr = (LONG)mui_get(bfound.obj, MUIA_Table_Active);
	int r = 0;

	if (id == ID_BPAIRED || id == ID_BFOUND)
		return;
	if (!x) {
		say("No Alice68 network board");
		return;
	}
	switch (id) {
	case ID_BSCAN:
		r = cc_bt_cmd(AN_BT_CMD_SCAN, NULL);
		break;
	case ID_BPAIR:
		if (fr < 0 || (ULONG)fr >= x->nfound) {
			say("Pair: Scan, then pick the device in the lower list");
			return;
		}
		r = cc_bt_cmd(AN_BT_CMD_PAIR, x->found[fr].addr);
		if (!r)
			say("Pairing - follow what the line above the lists says");
		break;
	case ID_BCONN:
	case ID_BFORGET:
		if (pr < 0 || (ULONG)pr >= x->nkeys) {
			say("Pick a paired device first");
			return;
		}
		if (id == ID_BFORGET && ask("Forget this Bluetooth device?\nIt has to be paired again to be used.", "*_Forget|_Cancel") != 1)
			return;
		r = cc_bt_cmd(id == ID_BCONN ? AN_BT_CMD_CONNECT : AN_BT_CMD_FORGET, x->keys[pr].addr);
		break;
	}
	if (r == -2)
		sayf("Bluetooth is not ready: %s", x->state == AN_BT_NOFW ? "its firmware has not arrived" : cc_bt_state(x->state));
}

/* the paired list changed: into cmdline.txt at once; the page drawn again when anything changed */
static void bt_tick(int p)
{
	volatile struct an_bt *x = btb();
	ULONG sg;

	if (!x)
		return;
	if (x->keys_gen != bt_gen_seen) {
		if (bt_gen_seen != ~0u && cs_ok) {
			int e = cc_bt_keep();
			if (e)
				sayf("Bluetooth: the pairing could not be kept in cmdline.txt: %s", e < 0 ? "it would be too long" : cc_err(e));
			else
				say("Bluetooth: the paired devices are kept in cmdline.txt");
		}
		bt_gen_seen = x->keys_gen;
	}
	sg = bt_signature(x);
	if (p == P_BT && sg != bt_sig) {
		bt_sig = sg;
		bt_show();
	} else if (p == P_BT)
		bt_log_show();
}

/* ==================================================================
 * the module
 * ================================================================== */
Object *card_page(int p)
{
	switch (p) {
	case P_CARD: return card_page_build();
	case P_FILES: return files_page_build();
	case P_SETTINGS: return settings_page_build();
	case P_TEXT: return text_files_build();
	case P_BT: return bt_page_build();
	}
	return NULL;
}

/* before the pages are built: the tables' rows */
static int rows_all(void)
{
	return rows_init(&parts, CS_MAXPART, 120) | rows_init(&used, CS_MAXUSED, 120) | rows_init(&files, CC_MAXF, 160) |
	       rows_init(&lines, CC_MAXL, CC_LINE + 8) | rows_init(&bpaired, AN_BT_KEYS, 120) |
	       rows_init(&bfound, AN_BT_FOUND, 120) | rows_init(&blog, 12, 120);
}

int card_prepare(void)
{
	int e = cc_open();

	if (rows_all() < 0 || e < 0)
		return -1;
	cs_ok = !e;
	if (cs_ok && !cc_load())
		loaded = 1;
	return 0;
}

void card_start(void)
{
	if (!cs_ok) {
		vset(&card_v, C_DEV, "alice68hdf.device's card service is not there (not Alice68, or too old)");
		return;
	}
	card_show(0);
	files_load();
	opts_show();
	lines_show();
	bt_tick(-1);
}

void card_stop(void)
{
	cc_close();
	rows_free(&parts);
	rows_free(&used);
	rows_free(&files);
	rows_free(&lines);
	rows_free(&bpaired);
	rows_free(&bfound);
	rows_free(&blog);
	if (fe)
		FreeVec(fe);
	fe = NULL;
}

/* Edit, Last Saved: cmdline.txt and config.txt as on the card again */
void card_reload(void)
{
	if (!cs_ok)
		return;
	loaded = !cc_load();
	card_start();
}

void card_shown(int p)
{
	opts_read();                                     /* what was typed on the page left */
	if (p == P_TEXT || p == P_SETTINGS) {
		opts_show();
		lines_show();
	} else if (p == P_BT) {
		bt_sig = 0;
		bt_tick(p);
	}
}

void card_tick(int p)
{
	bt_tick(p);
}

int card_changed(void)
{
	if (!cs_ok || !loaded)
		return 0;
	opts_read();
	return cc_changed();
}

int card_save(void)
{
	int f, r;

	if (!cs_ok || !loaded)
		return 0;
	opts_read();
	r = cc_save(&f);
	if (r < 0) {
		sayf("%s not saved: %s", f == 2 ? "config.txt" : "cmdline.txt", cc_err(-r));
		return -1;
	}
	opts_show();
	lines_show();
	return r;
}

int card_command(ULONG id)
{
	if (id >= ID_BT && id < ID_BT + 100) {
		bt_action(id);
		return 1;
	}
	if (id < ID_CARD || id >= ID_CARD + 100)
		return 0;
	if (!cs_ok) {
		say("alice68hdf.device's card service is not there: the SD card can't be changed from here");
		return 1;
	}
	if (id != ID_OPT)
		opts_read();                                 /* typed, not yet taken: before anything changes the values */
	switch (id) {
	case ID_FREE:
		say("Counting free space...");
		card_show(1);
		say("Free space counted");
		break;
	case ID_VOL:
		fvol = mui_get(f_vol, MUIA_Cycle_Active) ? CS_EXFAT : CS_FAT;
		path[0] = 0;
		files_load();
		break;
	case ID_FILTER:
		files_show();
		break;
	case ID_FILES:
		if (fsel() >= 0)
			mui_set(f_name, MUIA_String_Contents, (ULONG)fe[fsel()].name);
		break;
	case ID_LINES: case ID_LSET: case ID_LADD: case ID_LINS: case ID_LDEL:
		lines_action(id);
		break;
	case ID_CMDLINE:
		cc_cmdline_typed(str_of(t_cmd));
		opts_show();
		lines_show();
		say("cmdline.txt taken as typed; the Settings page follows it (Save writes it)");
		break;
	case ID_OPT:
		break;
	default:
		files_action(id);
		if (id == ID_BOOT || id == ID_KICK) {
			opts_show();
			lines_show();
		}
	}
	return 1;
}
