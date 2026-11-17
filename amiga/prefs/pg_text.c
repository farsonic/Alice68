/*
 * Alice68Prefs: the pages that show what Alice68's Shell command prints - Status (`Alice68
 * STATUS`, every second), Joystick (`Alice68 JOYTEST ONCE`, four times a second), Logs
 * (`Alice68 LOG` with LINES= and MATCH=, or the network board's log ring), Monitor (`Alice68
 * MONITOR DUMP`). The same code runs, its output caught (amiga/capture.c), so the page and the
 * Shell always agree; the tools' harnesses test that code.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ui.h"
#include "alice_net.h"

LONG a68_capture(LONG (*fn)(const char *, ULONG), const char *args, char *buf, ULONG max);
LONG status_main(const char *a, ULONG n);
LONG usblog_main(const char *a, ULONG n);
LONG joytest_main(const char *a, ULONG n);
LONG monitor_main(const char *a, ULONG n);
volatile unsigned char *cc_board(void);

enum { ID_LSRC = ID_TEXT, ID_LLINES, ID_LMATCH, ID_LFOLLOW, ID_LREFRESH, ID_MREFRESH, ID_MSAVE, ID_JPORT };

#define CAP 32768
static char *cap;                                   /* the captured text */
struct tview { Object *tbl; struct rows r; int split; };
static struct tview tv_status, tv_joy, tv_logs, tv_mon;
static Object *l_src, *l_lines, *l_match, *l_follow, *j_both;
static const char *const src_names[] = { "Kernel and USB driver", "Network and Bluetooth", NULL };
static const char *const lines_names[] = { "Last 100 lines", "Last 400 lines", "All", NULL };

/* text into a table: a row per line; split: runs of two or more spaces become columns */
static void show(struct tview *t, const char *s, int bottom)
{
	rows_begin(&t->r);
	while (*s) {
		char *d = rows_add(&t->r);
		int k = 0, cells = 1;
		if (!d)
			break;
		while (*s == ' ' && t->split)
			s++;
		while (*s && *s != '\n') {
			if (t->split && *s == ' ' && s[1] == ' ' && cells < 6) {
				while (*s == ' ')
					s++;
				if (*s && *s != '\n' && k < t->r.len - 1) {
					d[k++] = '\t';
					cells++;
				}
				continue;
			}
			if (k < t->r.len - 1)
				d[k++] = *s == '\t' ? ' ' : *s;
			s++;
		}
		d[k] = 0;
		if (*s)
			s++;
	}
	rows_end(&t->r);
	if (bottom && t->tbl)
		mui_set(t->tbl, MUIA_Table_Top, 100000);
}

static Object *tview(struct tview *t, const char *header, int split)
{
	t->split = split;
	if (rows_init(&t->r, 512, 160) < 0)
		return NULL;
	t->r.obj = t->tbl = table(header, 0, "Nothing to show.");
	return t->tbl;
}

/* ---- the network board's log ring: its last lines (WiFi, Ethernet, Bluetooth) ---- */
static void board_log(char *out, ULONG max, ULONG lines, const char *match)
{
	volatile unsigned char *nb = cc_board();
	ULONG wr, start, n = 0, i, nl = 0;
	volatile const char *r;
	ULONG ml = (ULONG)strlen(match);

	out[0] = 0;
	if (!nb) {
		rt_strlcpy(out, "No Alice68 network board: this is not an Alice68 kernel with the network.", max);
		return;
	}
	wr = ((volatile struct an_hdr *)nb)->log_wr;
	r = (volatile const char *)(nb + AN_OFF_LOG);
	start = wr > AN_LOG_BYTES ? wr - AN_LOG_BYTES : 0;
	for (i = start; i != wr && n + 2 < max; ) {             /* line by line, only those that match */
		char line[160];
		ULONG k = 0, j;
		int ok = !ml;
		while (i != wr && r[i % AN_LOG_BYTES] != '\n') {
			if (k < sizeof(line) - 1)
				line[k++] = r[i % AN_LOG_BYTES];
			i++;
		}
		if (i != wr)
			i++;
		line[k] = 0;
		for (j = 0; !ok && j + ml <= k; j++)
			ok = !rt_strncasecmp(line + j, match, ml);
		if (!ok)
			continue;
		for (j = 0; j < k && n + 2 < max; j++)
			out[n++] = line[j];
		out[n++] = '\n';
		out[n] = 0;
		nl++;
	}
	/* only the last lines */
	if (lines && nl > lines) {
		char *p = out;
		ULONG drop = nl - lines;
		while (drop && *p)
			if (*p++ == '\n')
				drop--;
		memmove(out, p, strlen(p) + 1);
	}
}

static void logs_load(void)
{
	static const ULONG nlines[] = { 100, 400, 0 };
	ULONG which = mui_get(l_lines, MUIA_Cycle_Active);
	ULONG lines = nlines[which < 3 ? which : 0];
	const char *m = str_of(l_match);

	if (!mui_get(l_src, MUIA_Cycle_Active)) {
		char args[120];
		rt_snprintf(args, sizeof(args), "%s%lu MATCH=\"%s\"", lines ? "LINES=" : "ALL ", (unsigned long)lines, m);
		if (!lines)
			rt_snprintf(args, sizeof(args), "ALL MATCH=\"%s\"", m);
		a68_capture(usblog_main, args, cap, CAP);
	} else
		board_log(cap, CAP, lines, m);
	show(&tv_logs, cap, 1);
}

static void monitor_load(void)
{
	a68_capture(monitor_main, "DUMP", cap, CAP);
	show(&tv_mon, cap, 0);
}

Object *text_page(int p)
{
	Object *g;

	if (!cap && !(cap = AllocVec(CAP, MEMF_ANY | MEMF_CLEAR)))
		return NULL;
	switch (p) {
	case P_STATUS:
		return page_full(tview(&tv_status, "Kernel status page\t\t\t\t\t", 1));
	case P_JOY:
		g = vgroup(6);
		j_both = checkbox(ID_JPORT);
		add(g, check_row(j_both, "The mouse port too (a mouse moving there shows as directions)"));
		add(g, tview(&tv_joy, "As games read the port\t\t\t\t\t", 1));
		return page_full(g);
	case P_LOGS:
		g = vgroup(6);
		l_src = cycle(src_names, ID_LSRC);
		l_lines = cycle(lines_names, ID_LLINES);
		l_match = string_id(60, ID_LMATCH);
		l_follow = checkbox(ID_LFOLLOW);
		if (!l_src || !l_lines || !l_match || !l_follow)
			return NULL;
		help(l_src, "Log", "Kernel and USB driver: the kernel's log ring (as on the splash) and alice68-usb.device's. "
		     "Network and Bluetooth: the network board's ring, where WiFi, Ethernet and Bluetooth write.");
		help(l_match, "Filter", "Only the lines with this text (any case), e.g. [BT] or wifi. Return applies it.");
		help(l_follow, "Follow", "Reads the log again every second and shows its end.");
		add(g, row(l_src, l_lines, label("Filter", QL_LABEL), l_match));
		add(g, row(check_row(l_follow, "Follow"), button("_Refresh", ID_LREFRESH), NULL, NULL));
		add(g, tview(&tv_logs, "Log\t", 0));
		return page_full(g);
	case P_MONITOR:
		g = vgroup(6);
		add(g, row(button("_Refresh", ID_MREFRESH), button("Sa_ve to RAM:", ID_MSAVE),
		           label("Tasks, backtraces and the board's logs, as MONITOR DUMP prints them", QL_NOTE), NULL));
		add(g, tview(&tv_mon, "Alice68 MONITOR DUMP\t\t\t\t\t", 1));
		return page_full(g);
	}
	return NULL;
}

/* the page came to the front */
void text_shown(int p)
{
	if (p == P_LOGS)
		logs_load();
	else if (p == P_MONITOR)
		monitor_load();
	else
		text_tick(p);
}

static int ticks;
/* four times a second */
void text_tick(int p)
{
	ticks++;
	if (p == P_JOY) {
		a68_capture(joytest_main, mui_get(j_both, MUIA_Selected) ? "ONCE MOUSEPORT" : "ONCE", cap, CAP);
		show(&tv_joy, cap, 0);
	} else if (p == P_STATUS && ticks % 4 == 0) {
		a68_capture(status_main, "", cap, CAP);
		show(&tv_status, cap, 0);
	} else if (p == P_LOGS && ticks % 4 == 0 && mui_get(l_follow, MUIA_Selected))
		logs_load();
}

int text_command(ULONG id)
{
	switch (id) {
	case ID_LSRC: case ID_LLINES: case ID_LMATCH: case ID_LREFRESH:
		logs_load();
		break;
	case ID_LFOLLOW:
		if (mui_get(l_follow, MUIA_Selected))
			logs_load();
		break;
	case ID_MREFRESH:
		monitor_load();
		say("Monitor: read again");
		break;
	case ID_MSAVE: {
		BPTR fh;
		monitor_load();
		fh = Open((CONST_STRPTR)"RAM:Alice68-Monitor.txt", MODE_NEWFILE);
		if (fh) {
			LONG n = (LONG)strlen(cap);
			say(Write(fh, cap, n) == n ? "Saved as RAM:Alice68-Monitor.txt" : "RAM:Alice68-Monitor.txt could not be written");
			Close(fh);
		} else
			say("RAM:Alice68-Monitor.txt could not be made");
		break;
	}
	case ID_JPORT:
		text_tick(P_JOY);
		break;
	default:
		return 0;
	}
	return 1;
}

void text_stop(void)
{
	rows_free(&tv_status.r);
	rows_free(&tv_joy.r);
	rows_free(&tv_logs.r);
	rows_free(&tv_mon.r);
	if (cap)
		FreeVec(cap);
	cap = NULL;
}
