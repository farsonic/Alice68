/*
 * Alice68Prefs 1.0 - Alice68's preferences editor (SYS:Prefs/Alice68Prefs), MUI 3.8 or later
 * (muimaster.library 19), in the look of Riptide's RiptidePrefs: its theme and classes are in
 * rt/ (see rt/README). One window: the page list on the left with its headings, the page's
 * head and contents on the right, Save, Use, Restart Pi and Cancel below, and a status line.
 * Every command is in the menus too. Keyboard: the underlined letters, P and Shift-P or the
 * cursor keys for the pages, Esc to cancel. A second start brings the window to the front.
 *
 *   About
 *   System       Status (the kernel's status page), Settings (cmdline.txt), Text files
 *   Storage      Card, Files
 *   Network      Status, Ethernet, WiFi, their settings and traffic
 *   Input        Bluetooth, Joystick
 *   Diagnostics  Logs, Monitor
 *
 * Save writes cmdline.txt and config.txt (the old ones kept as .bak) and the network's prefs;
 * Use applies the network settings without keeping them; Restart Pi asks the kernel to restart
 * through the network board. Without MUI it says so and points to the Shell's Alice68 command.
 *
 *   Alice68Prefs [PUBSCREEN=name] [PAGE=name] [THEME=DARK|LIGHT]
 * The icon's tool types are the same.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ui.h"
#include "gen/icon_calls.h"

static const char vers[] __attribute__((used)) = "$VER: Alice68Prefs " A68P_VERSION " (" A68P_DATE ")";

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *IconBase;

int card_prepare(void);
void card_reload(void);

/* ==================================================================
 * the pages (the nav list and the heads draw these)
 * ================================================================== */
const struct rp_page rp_pages[RP_NPAGES] = {
	[P_ABOUT] = { "About", "Alice68Prefs: which version this is, and where it comes from." },
	[P_STATUS] = { "Status", "The kernel's status page: the cores, temperature, the JIT, USB, the network. Live." },
	[P_SETTINGS] = { "Settings", "cmdline.txt's options, as the setup screen at boot has them. Save writes them; "
	                             "Restart Pi uses them." },
	[P_TEXT] = { "Text files", "config.txt line by line, and cmdline.txt as one line (the WiFi password hidden)." },
	[P_CARD] = { "Card", "The SD card's partitions, their free space, and what is mounted from it." },
	[P_FILES] = { "Files", "The FAT and exFAT partitions: delete, rename, new folder; a hardfile to boot from, a "
	                       "Kickstart ROM." },
	[P_NETSTATUS] = { "Status", "Both network units at a glance, and all their traffic of the last minute." },
	[P_ETH] = { "Ethernet", "The Pi's Gigabit Ethernet port (unit 1): the link, what each side offers, and the "
	                        "MAC address." },
	[P_WIFI] = { "WiFi", "The Pi's WiFi (unit 0): the network it is on, and the networks in range. Click one to "
	                     "join it." },
	[P_ETHSET] = { "Ethernet settings", "Speeds and duplex to offer, a fixed speed instead, flow control. Use "
	                                    "renegotiates the link; Save keeps it for the next start." },
	[P_WIFISET] = { "WiFi settings", "The network to join, its password (hidden unless Show is ticked), the "
	                                 "country and power saving." },
	[P_ETHTRAFFIC] = { "Ethernet traffic", "Frames and bytes in and out, drops and errors, and the last minute." },
	[P_WIFITRAFFIC] = { "WiFi traffic", "Frames and bytes in and out, drops and errors, and the last minute." },
	[P_BT] = { "Bluetooth", "Keyboards, mice and game pads: paired and found. Scan, pair, connect, forget." },
	[P_JOY] = { "Joystick", "The joystick port as games read it: directions and buttons, live." },
	[P_LOGS] = { "Logs", "The kernel's and the USB driver's logs, or the network board's (WiFi, Ethernet, "
	                     "Bluetooth). Filter, and follow." },
	[P_MONITOR] = { "Monitor", "Tasks, backtraces, the WiFi driver's state and the board's logs: what MONITOR "
	                           "DUMP prints." },
};
const int rp_nav_order[RP_NPAGES] = { P_ABOUT, P_STATUS, P_SETTINGS, P_TEXT, P_CARD, P_FILES, P_NETSTATUS, P_ETH,
                                      P_WIFI, P_ETHSET, P_WIFISET, P_ETHTRAFFIC, P_WIFITRAFFIC, P_BT, P_JOY, P_LOGS,
                                      P_MONITOR };
const char *const rp_nav_head[RP_NPAGES] = { NULL, "System", NULL, NULL, "Storage", NULL, "Network", NULL, NULL,
                                             NULL, NULL, NULL, NULL, "Input", NULL, "Diagnostics", NULL };

Object *app, *win, *status_line;
int page = P_ABOUT;
static Object *strip, *root, *nav, *head, *pages, *bar, *btn[4];

/* ==================================================================
 * About
 * ================================================================== */
static Object *about_page(void)
{
	Object *g = vgroup(6);

	add(g, left(label("Alice68Prefs " A68P_VERSION " (" A68P_DATE ")", QL_TITLE)));
	add(g, left(label("The preferences of Alice68, the standalone AGA machine on a\n"
	                  "Raspberry Pi 4: its SD card, cmdline.txt and config.txt, the\n"
	                  "network, Bluetooth, and what the kernel is doing.", QL_TEXT)));
	add(g, left(label("From the Shell", QL_TITLE)));
	add(g, left(label("C:Alice68 does the same without a window:\n"
	                  "Alice68 STATUS, CARD, BT, WIFI, LOG, MONITOR ... (Alice68 HELP).", QL_TEXT)));
	add(g, left(label("Licence and source", QL_TITLE)));
	add(g, left(label("Free software under the GNU General Public License,\n"
	                  "version 2. Its look and classes are Riptide's\n"
	                  "RiptidePrefs (github.com/farsonic/riptide).", QL_TEXT)));
	return page_box(g);
}

/* ==================================================================
 * the window
 * ================================================================== */
static Object *build_page(int p)
{
	switch (p) {
	case P_ABOUT: return about_page();
	case P_STATUS: case P_JOY: case P_LOGS: case P_MONITOR: return text_page(p);
	case P_SETTINGS: case P_TEXT: case P_CARD: case P_FILES: case P_BT: return card_page(p);
	}
	return net_page(p);
}

static struct NewMenu menus[] = {
	{ NM_TITLE, (CONST_STRPTR)"Project", 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Save", (CONST_STRPTR)"S", 0, 0, (APTR)ID_SAVE },
	{ NM_ITEM, (CONST_STRPTR)"Use", (CONST_STRPTR)"U", 0, 0, (APTR)ID_USE },
	{ NM_ITEM, (CONST_STRPTR)"Restart Pi...", (CONST_STRPTR)"R", 0, 0, (APTR)ID_RESTART },
	{ NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"About...", (CONST_STRPTR)"?", 0, 0, (APTR)ID_ABOUT },
	{ NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Quit", (CONST_STRPTR)"Q", 0, 0, (APTR)ID_QUIT },
	{ NM_TITLE, (CONST_STRPTR)"Edit", 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Last Saved", (CONST_STRPTR)"L", 0, 0, (APTR)ID_LASTSAVED },
	{ NM_TITLE, (CONST_STRPTR)"Settings", 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Dark Theme", (CONST_STRPTR)"T", CHECKIT | MENUTOGGLE, 0, (APTR)ID_DARK },
	{ NM_ITEM, (CONST_STRPTR)"Help Bubbles", (CONST_STRPTR)"H", CHECKIT | MENUTOGGLE, 0, (APTR)ID_TIPS },
	{ NM_END, 0, 0, 0, 0, 0 },
};

static Object *build_menus(void)
{
	int i;

	for (i = 0; menus[i].nm_Type != NM_END; i++) {
		if ((ULONG)menus[i].nm_UserData == ID_DARK)
			menus[i].nm_Flags = (UWORD)(CHECKIT | MENUTOGGLE | (theme_dark() ? CHECKED : 0));
		if ((ULONG)menus[i].nm_UserData == ID_TIPS)
			menus[i].nm_Flags = (UWORD)(CHECKIT | MENUTOGGLE | (theme_tips() ? CHECKED : 0));
	}
	return mui_make(MUIO_MenustripNM, (ULONG)menus, 0);
}

static int build_window(const char *pubscreen)
{
	static const char *const blabel[4] = { "_Save", "_Use", "_Restart Pi", "_Cancel" };
	static const ULONG bid[4] = { ID_SAVE, ID_USE, ID_RESTART, ID_CANCEL };
	static const char *const bhelp[4] = {
		"Writes cmdline.txt and config.txt if they changed (the old ones kept as .bak), and applies and keeps "
		"the network settings; joins the WiFi network typed in if it is not on it already.",
		"Applies the Ethernet and WiFi settings now, without keeping them. cmdline.txt and config.txt are not "
		"written: they only matter at the next start.",
		"Restarts the Pi (asks first when cmdline.txt or config.txt has changes that are not saved).",
		"Closes the window without saving. The card keeps running." };
	Object *right, *top;
	int i;

	nav = mui_newc(nav_mcc, MUIA_Nav_Active, (ULONG)page, MUIA_ShortHelp, (ULONG)" ", TAGEND);
	head = mui_newc(head_mcc, MUIA_Head_Page, (ULONG)page, TAGEND);
	pages = mui_new(MUIC_Group, MUIA_Group_PageMode, TRUE, TAGEND);
	status_line = mui_newc(status_mcc, TAGEND);
	bar = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_SameSize, TRUE, MUIA_Group_Spacing, 8,
	              MUIA_InnerLeft, 8, MUIA_InnerRight, 8, MUIA_InnerTop, theme_compact() ? 3 : 6,
	              MUIA_InnerBottom, theme_compact() ? 3 : 6, MUIA_Background, (ULONG)theme_spec(T_PANEL), TAGEND);
	if (!nav || !head || !pages || !status_line || !bar)
		return -1;
	for (i = 0; i < RP_NPAGES; i++) {
		Object *pg = build_page(i);
		if (!pg)
			return -1;
		add(pages, pg);
	}
	for (i = 0; i < 4; i++) {
		btn[i] = button(blabel[i], bid[i]);
		if (!btn[i])
			return -1;
		help(btn[i], blabel[i] + 1, bhelp[i]);
		add(bar, btn[i]);
	}
	mui_set(pages, MUIA_Group_ActivePage, (ULONG)page);
	right = mui_new(MUIC_Group, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)head, MUIA_Group_Child,
	                (ULONG)pages, TAGEND);
	top = right ? mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)nav,
	                      MUIA_Group_Child, (ULONG)right, TAGEND) : NULL;
	root = top ? mui_new(MUIC_Group, MUIA_Group_Spacing, 0, MUIA_InnerLeft, 0, MUIA_InnerRight, 0, MUIA_InnerTop, 0,
	                     MUIA_InnerBottom, 0, MUIA_Background, (ULONG)theme_spec(T_WINDOW), MUIA_Group_Child,
	                     (ULONG)top, MUIA_Group_Child, (ULONG)mui_newc(line_mcc, TAGEND), MUIA_Group_Child,
	                     (ULONG)bar, MUIA_Group_Child, (ULONG)status_line, TAGEND) : NULL;
	strip = build_menus();
	if (!root || !strip)
		return -1;
	win = mui_new(MUIC_Window, MUIA_Window_Title, (ULONG)A68P_TITLE, MUIA_Window_ID, 0x41363850UL /* A68P */,
	              MUIA_Window_Menustrip, (ULONG)strip, MUIA_Window_RootObject, (ULONG)root,
	              pubscreen ? MUIA_Window_PublicScreen : TAG_IGNORE, (ULONG)pubscreen, TAGEND);
	if (!win)
		return -1;
	add(app, win);
	mui_do(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (ULONG)app, 2, MUIM_Application_ReturnID, ID_CANCEL);
	mui_do(app, MUIM_Notify, MUIA_Application_DoubleStart, MUIV_EveryTime, (ULONG)app, 2,
	       MUIM_Application_ReturnID, ID_FRONT);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)pages, 3, MUIM_Set, MUIA_Group_ActivePage,
	       MUIV_TriggerValue);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)head, 3, MUIM_Set, MUIA_Head_Page,
	       MUIV_TriggerValue);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_PAGE);
	return 0;
}

static void page_shown(void)
{
	card_shown(page);
	text_shown(page);
	net_tick(page);
}

void go_page(int p)
{
	page = p;
	mui_set(nav, MUIA_Nav_Active, (ULONG)p);
	page_shown();
}

static void menu_check(ULONG id, int on)
{
	Object *m = (Object *)mui_do(strip, MUIM_FindUData, id);
	if (m)
		mui_nnset(m, MUIA_Menuitem_Checked, (ULONG)on);
}

static void set_tips(int on)
{
	theme_set_tips(on);
	say(on ? "Help bubbles on" : "Help bubbles off (the texts stay until the window opens again)");
}

/* the window again after the theme changed: pens are taken anew */
static void reopen(void)
{
	mui_set(win, MUIA_Window_Open, FALSE);
	mui_set(root, MUIA_Background, (ULONG)theme_spec(T_WINDOW));
	mui_set(bar, MUIA_Background, (ULONG)theme_spec(T_PANEL));
	mui_set(win, MUIA_Window_Open, TRUE);
}

static int save(int keep)
{
	int c = 0, n;

	if (keep) {
		c = card_save();
		if (c < 0)
			return -1;
	}
	n = net_save(keep);
	if (c > 0)
		sayf("Saved %s (old copies: .bak)%s. Restart the Pi to use it.",
		     c == 3 ? "cmdline.txt and config.txt" : c == 1 ? "cmdline.txt" : "config.txt",
		     n > 0 ? "; network applied and kept" : "");
	return n < 0 ? -1 : 0;
}

/* 1: quit */
static int command(ULONG id)
{
	switch (id) {
	case ID_CANCEL:
	case ID_QUIT:
		if (card_changed()) {
			int a = ask("cmdline.txt or config.txt has changes that are not saved.\nClose without them?",
			            "*_Close|_Save first|C_ancel");
			if (!a || (a == 2 && card_save() < 0))
				return 0;
		}
		return 1;
	case ID_FRONT:
		mui_do(win, MUIM_Window_ToFront);
		mui_set(win, MUIA_Window_Activate, TRUE);
		break;
	case ID_PAGE:
		page = (int)mui_get(nav, MUIA_Nav_Active);
		page_shown();
		break;
	case ID_SAVE:
		save(1);
		break;
	case ID_USE:
		save(0);
		break;
	case ID_RESTART:
		if (card_changed()) {
			int a = ask("cmdline.txt or config.txt has changes that are not saved.", "_Save and restart|_Restart without them|_Cancel");
			if (!a || (a == 1 && card_save() < 0))
				break;
		} else if (ask("Restart the Pi now?\nAmigaOS stops at once: close your files first.", "*_Restart|_Cancel") != 1)
			break;
		{
			extern int cc_restart(void);
			if (cc_restart() < 0)
				say("No Alice68 network board: restart the Pi by hand (or Ctrl + Left Shift + F11)");
			else
				say("Restarting the Pi...");
		}
		break;
	case ID_ABOUT:
		go_page(P_ABOUT);
		break;
	case ID_LASTSAVED:
		card_reload();
		say("cmdline.txt and config.txt read again from the card");
		break;
	case ID_DARK:
		theme_set_dark(!theme_dark());
		menu_check(ID_DARK, theme_dark());
		reopen();
		break;
	case ID_TIPS:
		set_tips(!theme_tips());
		menu_check(ID_TIPS, theme_tips());
		break;
	default:
		if (!net_command(id) && !card_command(id))
			text_command(id);
	}
	return 0;
}

/* a quarter of a second */
static void tick_send(struct timerequest *t)
{
	t->tr_node.io_Command = TR_ADDREQUEST;
	t->tr_time.tv_secs = 0;
	t->tr_time.tv_micro = 250000;
	SendIO((struct IORequest *)t);
}

/* ==================================================================
 * arguments and tool types
 * ================================================================== */
static char lc(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* PAGE=: a page's title (the first one of that name: "Status" is the kernel's), a heading's
 * next page ("Network"), or its number */
static int page_arg(const char *s)
{
	int i;

	if (!s || !*s)
		return -1;
	if (*s >= '0' && *s <= '9') {
		const char *e;
		unsigned long v = rt_strtoul(s, &e, 10);
		return !*e && v < RP_NPAGES ? (int)v : -1;
	}
	for (i = 0; i < RP_NPAGES; i++) {
		int p = rp_nav_order[i];
		if (!rt_strcasecmp(rp_pages[p].title, s) || (rp_nav_head[i] && !rt_strcasecmp(rp_nav_head[i], s)))
			return p;
	}
	return -1;
}

static char a_pub[64], a_page[32], a_theme[8];

static void wb_tooltypes(struct WBStartup *wb)
{
	struct DiskObject *dob;
	BPTR old;
	const char *v;

	if (!wb || wb->sm_NumArgs < 1 || !wb->sm_ArgList)
		return;
	IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36);
	if (!IconBase)
		return;
	old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
	dob = GetDiskObject((CONST_STRPTR)wb->sm_ArgList[0].wa_Name);
	if (dob) {
		CONST_STRPTR *tt = (CONST_STRPTR *)dob->do_ToolTypes;
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"PUBSCREEN")) != NULL)
			rt_strlcpy(a_pub, v, sizeof(a_pub));
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"PAGE")) != NULL)
			rt_strlcpy(a_page, v, sizeof(a_page));
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"THEME")) != NULL)
			rt_strlcpy(a_theme, v, sizeof(a_theme));
		FreeDiskObject(dob);
	}
	CurrentDir(old);
	CloseLibrary(IconBase);
	IconBase = NULL;
}

/* PUBSCREEN/K,PAGE/K,THEME/K: 0, or -1 when the line is wrong */
static int cli_args(void)
{
	LONG a[3] = { 0, 0, 0 };
	APTR rda = ReadArgs((CONST_STRPTR)"PUBSCREEN/K,PAGE/K,THEME/K", a, NULL);

	if (!rda) {
		PrintFault(IoErr(), (CONST_STRPTR)"Alice68Prefs");
		return -1;
	}
	if (a[0])
		rt_strlcpy(a_pub, (const char *)a[0], sizeof(a_pub));
	if (a[1])
		rt_strlcpy(a_page, (const char *)a[1], sizeof(a_page));
	if (a[2])
		rt_strlcpy(a_theme, (const char *)a[2], sizeof(a_theme));
	FreeArgs(rda);
	return 0;
}

long amiga_main(char *args, long len)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	struct WBStartup *wb = NULL;
	struct MsgPort *tport = NULL;
	struct timerequest *treq = NULL;
	int timer_on = 0, r = 20, p;
	ULONG sigs = 0;

	(void)args; (void)len;
	if (!me->pr_CLI) {
		WaitPort(&me->pr_MsgPort);
		wb = (struct WBStartup *)GetMsg(&me->pr_MsgPort);
	}
	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
	if (!DOSBase || !IntuitionBase || !GfxBase)
		goto out;
	if (wb)
		wb_tooltypes(wb);
	else if (cli_args() < 0) {
		r = 10;
		goto out;
	}
	MUIMasterBase = OpenLibrary((CONST_STRPTR)"muimaster.library", 19);
	if (!MUIMasterBase) {
		static const char t[] = "Alice68Prefs needs MUI 3.8 or later (muimaster.library 19).\n"
		                        "Without MUI, the Shell's Alice68 command does the same: Alice68 HELP lists it.\n";
		if (!wb)
			Write(Output(), (APTR)t, (LONG)sizeof(t) - 1);
		else {
			struct EasyStruct es = { sizeof(struct EasyStruct), 0, (CONST_STRPTR)"Alice68Prefs",
			                         (CONST_STRPTR)"Alice68Prefs needs MUI 3.8 or later (muimaster.library 19).\n"
			                         "Without MUI, the Shell's Alice68 command does the same:\n"
			                         "Alice68 HELP lists it.", (CONST_STRPTR)"OK" };
			EasyRequestArgs(NULL, &es, NULL, NULL);
		}
		goto out;
	}
	if (a_theme[0])
		theme_set_dark(lc(a_theme[0]) == 'd');
	p = page_arg(a_page);
	if (p >= 0)
		page = p;
	{
		struct Screen *s = LockPubScreen(a_pub[0] ? (CONST_STRPTR)a_pub : NULL);
		if (s) {
			theme_set_compact(s->Height < 400);
			UnlockPubScreen(NULL, s);
		}
	}
	if (rp_classes_make() < 0 || widgets_classes_make() < 0)
		goto out;
	app = mui_new(MUIC_Application, MUIA_Application_Title, (ULONG)"Alice68Prefs", MUIA_Application_Version,
	              (ULONG)vers, MUIA_Application_Copyright, (ULONG)"GPL-2.0", MUIA_Application_Author,
	              (ULONG)"The Alice68 and Riptide authors",
	              MUIA_Application_Description, (ULONG)"Alice68's preferences",
	              MUIA_Application_Base, (ULONG)"ALICE68PREFS", MUIA_Application_UseCommodities, FALSE,
	              MUIA_Application_UseRexx, FALSE, MUIA_Application_SingleTask, TRUE, TAGEND);
	if (!app) {
		r = MUI_Error() == MUIE_SingleTask ? 0 : 20;       /* another one is open: it came to the front */
		goto out;
	}
	if (card_prepare() < 0 || build_window(a_pub[0] ? a_pub : NULL) < 0)
		goto out;
	net_start();
	card_start();
	mui_set(win, MUIA_Window_Open, TRUE);
	if (!mui_get(win, MUIA_Window_Open))
		goto out;
	say(net_hello() ? net_hello() : "Alice68Prefs " A68P_VERSION);
	page_shown();
	tport = CreateMsgPort();
	treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest)) : NULL;
	if (treq && !OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)treq, 0))
		timer_on = 1;
	if (timer_on)
		tick_send(treq);
	r = 0;
	for (;;) {
		LONG id = (LONG)mui_do(app, MUIM_Application_NewInput, (ULONG)&sigs);
		if (id == MUIV_Application_ReturnID_Quit)
			break;
		if (id && command((ULONG)id))
			break;
		if (sigs) {
			ULONG got = Wait(sigs | SIGBREAKF_CTRL_C | net_signals() | (timer_on ? 1UL << tport->mp_SigBit : 0));
			if (got & SIGBREAKF_CTRL_C)
				break;
			net_signal(got);
			if (timer_on && (got & (1UL << tport->mp_SigBit)) && CheckIO((struct IORequest *)treq)) {
				static int q;
				WaitIO((struct IORequest *)treq);
				text_tick(page);
				if (++q % 4 == 0) {
					net_tick(page);
					card_tick(page);
				}
				tick_send(treq);
			}
			sigs = got & sigs;
		}
	}
out:
	if (timer_on) {
		if (!CheckIO((struct IORequest *)treq))
			AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
		CloseDevice((struct IORequest *)treq);
	}
	if (treq)
		DeleteIORequest(treq);
	if (tport)
		DeleteMsgPort(tport);
	if (app) {
		if (win)
			mui_set(win, MUIA_Window_Open, FALSE);
		MUI_DisposeObject(app);
	}
	net_stop();
	card_stop();
	text_stop();
	ui_free();
	widgets_classes_free();
	rp_classes_free();
	if (MUIMasterBase)
		CloseLibrary(MUIMasterBase);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	if (DOSBase)
		CloseLibrary((struct Library *)DOSBase);
	if (wb) {
		Forbid();
		ReplyMsg((struct Message *)wb);
	}
	return r;
}
