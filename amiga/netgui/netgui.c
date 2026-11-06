/*
 * Alice68-Network 2.1 - the settings and statistics of Alice68's network
 * card (alice68net.device: unit 0 the Pi 4's WiFi, unit 1 its Gigabit
 * Ethernet, both run by the kernel on CPU1). MUI, in the look of Riptide's
 * RiptidePrefs: its theme and classes are in rt/ (see rt/README).
 *
 * The page list: About and Status; Networks (what the Ethernet and the WiFi
 * are doing, and the WiFi networks in range); Settings (the Ethernet's
 * speeds, duplex, autonegotiation and flow control; the WiFi's network,
 * password, country and power saving); Traffic (each unit's numbers and
 * the last minute as a graph). Save, Apply and Close below, and a status
 * line. The numbers come straight from the network board, once a second;
 * actions go to alice68net.device without waiting (the window never blocks).
 *
 * What it keeps: ENV:alice68wifi.prefs and ENVARC: (the WiFi network: SSID,
 * PASSWORD, COUNTRY - also read by "Alice68-WiFi RECONNECT") and
 * ENV:alice68net.prefs and ENVARC: (ETH= and WIFI=, which alice68net.device
 * applies when it starts). The MAC addresses are the kernel's business:
 * Commodore's OUI 00:80:10, unless net.oui=off is in cmdline.txt.
 *
 * Start: from the Shell or Workbench, no arguments.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui_classes.h"
#include "prefs/mui_theme.h"
#include "prefs/mui_widgets.h"
#include "prefs/stat_util.h"
#include "prefs/prefs_fields.h"
#include "gen/expansion_calls.h"
#include "alice_net.h"
#include "wifi_api.h"

#define A68N_VERSION "2.1"
#define A68N_DATE    "07.11.2026"
#define A68N_TITLE   "Alice68 Network"
static const char vers[] __attribute__((used)) = "$VER: Alice68-Network " A68N_VERSION " (" A68N_DATE ")";

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *ExpansionBase;

#define SEC_OPEN  0x01                  /* the kernel's SEC_* (wifi/fmac.h) */
#define SEC_WEP   0x02
#define SEC_WPA   0x04
#define SEC_WPA2  0x08
#define SEC_WPA3  0x10
#define SEC_8021X 0x20

/* ==================================================================
 * the pages (the nav list and the heads draw these)
 * ================================================================== */
const struct rp_page rp_pages[RP_NPAGES] = {
	[P_ABOUT] = { "About", "Alice68-Network: which version this is, and where it comes from." },
	[P_STATUS] = { "Status", "Both network units at a glance, and all their traffic of the last minute." },
	[P_ETH] = { "Ethernet", "The Pi's Gigabit Ethernet port (unit 1): the link, what each side offers, "
	                        "and the MAC address." },
	[P_WIFI] = { "WiFi", "The Pi's WiFi (unit 0): the network it is on, and the networks in range. "
	                     "Click one to join it." },
	[P_ETHSET] = { "Ethernet", "Speeds and duplex to offer, a fixed speed instead, flow control. Apply "
	                           "renegotiates the link; Save keeps it for the next start." },
	[P_WIFISET] = { "WiFi", "The network to join, its password (hidden unless Show is ticked), the "
	                        "country and power saving." },
	[P_ETHTRAFFIC] = { "Ethernet", "Frames and bytes in and out, drops and errors, and the last minute." },
	[P_WIFITRAFFIC] = { "WiFi", "Frames and bytes in and out, drops and errors, and the last minute." },
};
const int rp_nav_order[RP_NPAGES] = { P_ABOUT, P_STATUS, P_ETH, P_WIFI, P_ETHSET, P_WIFISET, P_ETHTRAFFIC,
                                      P_WIFITRAFFIC };
const char *const rp_nav_head[RP_NPAGES] = { NULL, NULL, "Networks", NULL, "Settings", NULL, "Traffic", NULL };

/* return IDs */
enum {
	ID_SAVE = 1, ID_APPLY, ID_CLOSE, ID_PAGE, ID_FRONT,
	ID_ETH_ON, ID_ETH_OFF, ID_AUTONEG, ID_WIFI_ON, ID_WIFI_OFF,
	ID_JOIN, ID_DISC, ID_SCAN, ID_PICK, ID_SHOWPW
};

static Object *app, *win, *root, *nav, *head, *pages, *status, *bar, *btn[3];
static int page = P_STATUS;

/* ==================================================================
 * the network board: the kernel's numbers, read without asking anyone
 * ================================================================== */
static volatile UBYTE *nb;
static volatile struct an_unit *U(int u) { return (volatile struct an_unit *)(nb + AN_OFF_UNIT(u)); }

/* seconds since the Pi started, as the kernel last wrote it */
static ULONG boot_s(void)
{
	return nb ? ((volatile struct an_hdr *)nb)->boot_us / 1000000u : 0;
}

static void find_board(void)
{
	APTR cd;
	volatile struct an_hdr *h;

	ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 37);
	if (!ExpansionBase)
		return;
	cd = FindConfigDev(NULL, AN_MANUFACTURER, AN_PRODUCT);
	if (cd) {
		h = *(volatile struct an_hdr **)((UBYTE *)cd + 32);        /* struct ConfigDev: cd_BoardAddr */
		if (h && h->magic == AN_MAGIC && h->version == AN_VERSION)
			nb = (volatile UBYTE *)h;
	}
	CloseLibrary(ExpansionBase);
	ExpansionBase = NULL;
}

/* ==================================================================
 * the device: requests that never block the window
 * ================================================================== */
static struct MsgPort *ioport;
enum { R_WIFI, R_SCAN, R_WCFG, R_ETH, R_ECFG, NREQ };
static struct IOStdReq *rq[NREQ];
static int ropen[NREQ], rbusy[NREQ];
static UWORD rcmd[NREQ];
static struct WifiScanEntry scanbuf[AN_SCAN_MAX];
static struct WifiJoin joinbuf;
static struct NetEthCfg ecfg;
static ULONG wcfg;
static char note[120];                  /* what the last action did, for the status line */

static void dev_open(void)
{
	int i;

	ioport = CreateMsgPort();
	if (!ioport)
		return;
	for (i = 0; i < NREQ; i++) {
		rq[i] = (struct IOStdReq *)CreateIORequest(ioport, sizeof(struct IOStdReq));
		if (rq[i] && !OpenDevice((CONST_STRPTR)"alice68net.device", i >= R_ETH ? 1 : 0, (struct IORequest *)rq[i], 0))
			ropen[i] = 1;
	}
}

static void dev_close(void)
{
	int i;

	for (i = 0; i < NREQ; i++) {
		if (rq[i] && ropen[i]) {
			if (rbusy[i]) {
				AbortIO((struct IORequest *)rq[i]);
				WaitIO((struct IORequest *)rq[i]);
			}
			CloseDevice((struct IORequest *)rq[i]);
		}
		if (rq[i])
			DeleteIORequest(rq[i]);
	}
	if (ioport)
		DeleteMsgPort(ioport);
}

/* 0: sent; -1: that request is still busy, or the device is not there */
static int dev_send(int r, UWORD cmd, APTR data, ULONG len)
{
	if (!ropen[r] || rbusy[r])
		return -1;
	rq[r]->io_Command = cmd;
	rq[r]->io_Data = data;
	rq[r]->io_Length = len;
	rq[r]->io_Error = 0;
	rcmd[r] = cmd;
	SendIO((struct IORequest *)rq[r]);
	rbusy[r] = 1;
	return 0;
}

/* ==================================================================
 * formatting
 * ================================================================== */
static const char *secname(ULONG s)
{
	return s & SEC_WPA3 ? (s & SEC_WPA2 ? "WPA2/WPA3" : "WPA3") : s & SEC_WPA2 ? "WPA2" : s & SEC_WPA ? "WPA" :
	       s & SEC_8021X ? "Enterprise" : s & SEC_WEP ? "WEP" : "open";
}

static void fmt_mac(char *buf, int len, const volatile UBYTE *m)
{
	rt_snprintf(buf, (size_t)len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int mac_zero(const volatile UBYTE *m)
{
	return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

/* the MAC address, and whose it is */
static void fmt_mac_full(char *buf, int len, volatile struct an_unit *x, const char *none)
{
	if (mac_zero(x->mac)) {
		rt_strlcpy(buf, none, (size_t)len);
		return;
	}
	fmt_mac(buf, len, x->mac);
	if (x->mac[0] == (UBYTE)(AN_OUI_COMMODORE >> 16) && x->mac[1] == (UBYTE)(AN_OUI_COMMODORE >> 8) &&
	    x->mac[2] == (UBYTE)AN_OUI_COMMODORE)
		rt_strlcpy(buf + strlen(buf), "  Commodore", (size_t)len - strlen(buf));
}

/* what one side of an Ethernet link advertises: "10M half, 10M full, ..." */
static void fmt_modes(char *buf, int len, ULONG m)
{
	static const char *const names[8] = { "10 half", "10 full", "100 half", "100 full", "1G half", "1G full",
	                                      "pause", "asym. pause" };
	int i, n = 0;

	buf[0] = 0;
	for (i = 0; i < 8; i++)
		if (m & (1UL << i))
			n += rt_snprintf(buf + n, (size_t)(len - n > 0 ? len - n : 0), "%s%s", n ? ", " : "", names[i]);
	if (!n)
		rt_strlcpy(buf, "-", (size_t)len);
}

/* a value cut to fit a line (the labels do not wrap) */
static void cut(char *b, int max)
{
	if ((int)strlen(b) > max && max > 3) {
		b[max - 3] = '.';
		b[max - 2] = '.';
		b[max - 1] = '.';
		b[max] = 0;
	}
}

/* ==================================================================
 * building blocks
 * ================================================================== */
static Object *rect(void)
{
	return mui_new(MUIC_Rectangle, TAGEND);
}

static Object *hgroup2(Object *a, Object *b)
{
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)a,
	               MUIA_Group_Child, (ULONG)b, TAGEND);
}

static Object *label(const char *text, int kind)
{
	return mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)text, MUIA_QLabel_Kind, (ULONG)kind, TAGEND);
}

static Object *left(Object *o)
{
	return o ? hgroup2(o, rect()) : NULL;
}

static Object *button(const char *text, ULONG id)
{
	const char *u = strchr(text, '_');
	char key = u && u[1] ? (char)(u[1] >= 'A' && u[1] <= 'Z' ? u[1] + 32 : u[1]) : 0;
	Object *b = mui_newc(qbutton_mcc, MUIA_QButton_Label, (ULONG)text, MUIA_InputMode, MUIV_InputMode_RelVerify,
	                     MUIA_ControlChar, (ULONG)key, MUIA_CycleChain, 1, TAGEND);

	if (b && app)
		mui_do(b, MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID, id);
	return b;
}

static Object *checkbox(ULONG id)
{
	Object *g = mui_new(MUIC_Image, MUIA_Frame, MUIV_Frame_ImageButton, MUIA_InputMode, MUIV_InputMode_Toggle,
	                    MUIA_Image_Spec, MUII_CheckMark, MUIA_Image_FreeVert, FALSE, MUIA_Background, MUII_ButtonBack,
	                    MUIA_ShowSelState, FALSE, MUIA_CycleChain, 1, TAGEND);

	if (g && id)
		mui_do(g, MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, id);
	return g;
}

/* a check box with its text right of it, the rest of the row empty */
static Object *check_row(Object *cb, const char *text)
{
	return cb ? mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 6, MUIA_Group_Child, (ULONG)cb,
	                    MUIA_Group_Child, (ULONG)label(text, QL_LABEL), MUIA_Group_Child, (ULONG)rect(), TAGEND)
	          : NULL;
}

static Object *string(ULONG maxlen, int secret)
{
	return mui_new(MUIC_String, MUIA_Frame, MUIV_Frame_String, MUIA_String_MaxLen, maxlen,
	               secret ? MUIA_String_Secret : TAG_IGNORE, TRUE, MUIA_CycleChain, 1, TAGEND);
}

static Object *row(Object *a, Object *b, Object *c, Object *d)
{
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8, MUIA_Group_Child, (ULONG)a,
	               b ? MUIA_Group_Child : TAG_IGNORE, (ULONG)b, c ? MUIA_Group_Child : TAG_IGNORE, (ULONG)c,
	               d ? MUIA_Group_Child : TAG_IGNORE, (ULONG)d, MUIA_Group_Child, (ULONG)rect(), TAGEND);
}

/* a page's contents with the theme's margins */
static Object *page_box(Object *c)
{
	ULONG in = theme_compact() ? 4 : 10;

	return c ? mui_new(MUIC_Group, MUIA_InnerLeft, 12, MUIA_InnerRight, 12, MUIA_InnerTop, in, MUIA_InnerBottom, in,
	                   MUIA_Group_VertSpacing, theme_compact() ? 3 : 6, MUIA_Group_Child, (ULONG)c,
	                   MUIA_Group_Child, (ULONG)rect(), TAGEND) : NULL;
}

static Object *vgroup(int spacing)
{
	return mui_new(MUIC_Group, MUIA_Group_VertSpacing, theme_compact() ? spacing / 2 : spacing, TAGEND);
}

static void add(Object *g, Object *o)
{
	if (g && o)
		mui_do(g, OM_ADDMEMBER, (ULONG)o);
}

/* A block of "name  value" rows, as MikroTik's Status tab: names right-aligned
 * in the first column, the values (which change) in the others. */
#define VMAX 14
#define VLEN 64                         /* longer values are cut */
struct vblock {
	int n;
	Object *obj[VMAX];
	char text[VMAX][VLEN], shown[VMAX][VLEN];
};

static Object *vblock_build(struct vblock *v, const char *const *names, int n, int cols)
{
	Object *g = mui_new(MUIC_Group, MUIA_Group_Columns, cols, MUIA_Group_HorizSpacing, 12,
	                    MUIA_Group_VertSpacing, theme_compact() ? 1 : 3, TAGEND);
	int i, c;

	if (!g)
		return NULL;
	v->n = n;
	for (i = 0; i < n; i++) {
		Object *l = label(names[i], QL_LABEL);
		if (!l)
			return NULL;
		add(g, l);
		for (c = 1; c < cols; c++) {
			int k = i * (cols - 1) + (c - 1);
			v->text[k][0] = '-';
			v->text[k][1] = 0;
			v->obj[k] = label(v->text[k], QL_NOTE);
			if (!v->obj[k])
				return NULL;
			add(g, v->obj[k]);
		}
	}
	return g;
}

/* show a value; only what changed is drawn again */
static void vset(struct vblock *v, int k, const char *s)
{
	if (!v->obj[k])
		return;
	rt_strlcpy(v->text[k], s, VLEN);
	cut(v->text[k], VLEN - 1 < 52 ? VLEN - 1 : 52);
	if (strcmp(v->text[k], v->shown[k])) {
		rt_strlcpy(v->shown[k], v->text[k], VLEN);
		mui_set(v->obj[k], MUIA_QLabel_Text, (ULONG)v->text[k]);
	}
}

/* ==================================================================
 * traffic: rates from the counters, a minute of history per unit
 * ================================================================== */
struct traffic {
	ULONG rx_b, tx_b, rx_p, tx_p;       /* the counters last time */
	ULONG rate_rxb, rate_txb, rate_rxp, rate_txp;
	int valid;
	struct rp_hist hist;
};
static struct traffic trf[2], trf_all;

static void traffic_sample(int u, ULONG ms)
{
	volatile struct an_unit *x = U(u);
	struct traffic *t = &trf[u];
	ULONG rb = x->rx_bytes, tb = x->tx_bytes, rp = x->rx_pkts, tp = x->tx_pkts;

	if (t->valid && ms) {
		t->rate_rxb = (rb - t->rx_b) / ms * 1000u + (rb - t->rx_b) % ms * 1000u / ms;
		t->rate_txb = (tb - t->tx_b) / ms * 1000u + (tb - t->tx_b) % ms * 1000u / ms;
		t->rate_rxp = (rp - t->rx_p) * 1000u / ms;
		t->rate_txp = (tp - t->tx_p) * 1000u / ms;
	}
	t->rx_b = rb; t->tx_b = tb; t->rx_p = rp; t->tx_p = tp;
	t->valid = 1;
	rp_hist_push(&t->hist, t->rate_rxb, t->rate_txb);
}

/* ==================================================================
 * About
 * ================================================================== */
static Object *about_page(void)
{
	Object *g = vgroup(6);

	add(g, left(label("Alice68-Network " A68N_VERSION " (" A68N_DATE ")", QL_TITLE)));
	add(g, left(label("The settings and numbers of Alice68's network card:\n"
	                  "the Raspberry Pi 4's WiFi and Gigabit Ethernet, run by\n"
	                  "the kernel on CPU1, used through alice68net.device.", QL_TEXT)));
	add(g, left(label("MAC addresses", QL_TITLE)));
	add(g, left(label("Both units' addresses start with Commodore International's\n"
	                  "00:80:10; the last three bytes are the Pi's own. net.oui=off\n"
	                  "in cmdline.txt keeps the Pi's own addresses.", QL_TEXT)));
	add(g, left(label("Licence and source", QL_TITLE)));
	add(g, left(label("Free software under the GNU General Public License,\n"
	                  "version 2. Its look and classes are Riptide's\n"
	                  "RiptidePrefs (github.com/farsonic/riptide).", QL_TEXT)));
	return page_box(g);
}

/* ==================================================================
 * Status
 * ================================================================== */
enum { S_ETH, S_ETHRATE, S_WIFI, S_WIFIRATE, S_N };
static const char *const st_names[S_N] = { "Ethernet", "in / out", "WiFi", "in / out" };
static struct vblock st_v;
static Object *st_graph;

static Object *status_page(void)
{
	Object *g = vgroup(10);

	st_graph = mui_newc(graph_mcc, MUIA_Graph_Hist, (ULONG)&trf_all.hist, MUIA_Graph_Kind, GK_BYTES,
	                    MUIA_Graph_Title, (ULONG)"All traffic", MUIA_Graph_Lines, 7, TAGEND);
	add(g, vblock_build(&st_v, st_names, S_N, 2));
	add(g, st_graph);
	return st_graph ? page_box(g) : NULL;
}

static void status_refresh(void)
{
	volatile struct an_unit *e = U(1), *w = U(0);
	char b[VLEN], i1[24], o1[24];

	if (e->kind == AN_KIND_NONE)
		rt_strlcpy(b, "not in use (eth=off)", sizeof(b));
	else if (e->link)
		rt_snprintf(b, sizeof(b), "up, %lu Mbit/s %s duplex", (unsigned long)e->speed_mbps,
		            (e->link_flags & AN_LF_FULL) ? "full" : "half");
	else
		rt_strlcpy(b, e->state == AN_ST_OFF ? "off" : "no link", sizeof(b));
	vset(&st_v, S_ETH, b);
	rp_fmt_rate(i1, sizeof(i1), trf[1].rate_rxb);
	rp_fmt_rate(o1, sizeof(o1), trf[1].rate_txb);
	rt_snprintf(b, sizeof(b), "%s / %s", i1, o1);
	vset(&st_v, S_ETHRATE, b);
	if (w->kind == AN_KIND_NONE)
		rt_strlcpy(b, "not in use", sizeof(b));
	else if (w->state == AN_ST_CONNECTED)
		rt_snprintf(b, sizeof(b), "%s, %ld dBm, %lu Mbit/s", (const char *)w->ssid, (long)w->rssi,
		            (unsigned long)w->speed_mbps);
	else
		rt_strlcpy(b, w->msg[0] ? (const char *)w->msg : "off", sizeof(b));
	vset(&st_v, S_WIFI, b);
	rp_fmt_rate(i1, sizeof(i1), trf[0].rate_rxb);
	rp_fmt_rate(o1, sizeof(o1), trf[0].rate_txb);
	rt_snprintf(b, sizeof(b), "%s / %s", i1, o1);
	vset(&st_v, S_WIFIRATE, b);
	mui_set(st_graph, MUIA_Graph_Hist, (ULONG)&trf_all.hist);
}

/* ==================================================================
 * Networks: Ethernet
 * ================================================================== */
enum { E_STATE, E_RATE, E_AUTONEG, E_ADV, E_LPA, E_FLOW, E_DOWNS, E_UP, E_MAC, E_FMAC, E_N };
static const char *const eth_names[E_N] = { "State", "Rate", "Autonegotiation", "Offered here", "Offered there",
                                            "Flow control", "Link downs", "Link up for", "MAC address",
                                            "The Pi's own" };
static struct vblock eth_v;

static Object *eth_page(void)
{
	return page_box(vblock_build(&eth_v, eth_names, E_N, 2));
}

static void eth_refresh(void)
{
	volatile struct an_unit *x = U(1);
	char b[VLEN];
	ULONG f = x->link_flags;

	if (x->kind == AN_KIND_NONE) {
		vset(&eth_v, E_STATE, "not in use (eth=off in cmdline.txt)");
		return;
	}
	vset(&eth_v, E_STATE, x->link ? "up" : x->state == AN_ST_OFF ? "off" : x->state == AN_ST_FAILED ?
	                      "FAILED" : "no link");
	if (x->link) {
		rt_snprintf(b, sizeof(b), "%lu Mbit/s, %s duplex", (unsigned long)x->speed_mbps, (f & AN_LF_FULL) ? "full" : "half");
		vset(&eth_v, E_RATE, b);
		vset(&eth_v, E_AUTONEG, (f & AN_LF_AUTONEG) ? "done" : "off (fixed speed)");
		fmt_modes(b, sizeof(b), x->link_adv);
		vset(&eth_v, E_ADV, b);
		fmt_modes(b, sizeof(b), x->link_lpa);
		vset(&eth_v, E_LPA, b);
		vset(&eth_v, E_FLOW, (f & AN_LF_RXPAUSE) ? "on (pause frames)" : "off");
	} else {
		static const int k[] = { E_RATE, E_AUTONEG, E_ADV, E_LPA, E_FLOW };
		int i;
		for (i = 0; i < 5; i++)
			vset(&eth_v, k[i], "-");
	}
	rt_snprintf(b, sizeof(b), "%lu", (unsigned long)x->link_downs);
	vset(&eth_v, E_DOWNS, b);
	if (x->link && x->link_up_s && boot_s() >= x->link_up_s)
		rp_fmt_uptime(b, sizeof(b), boot_s() - x->link_up_s);
	else
		rt_strlcpy(b, "-", sizeof(b));
	vset(&eth_v, E_UP, b);
	fmt_mac_full(b, sizeof(b), x, "- (known once the Ethernet has started)");
	vset(&eth_v, E_MAC, b);
	if (mac_zero(x->factory_mac))
		rt_strlcpy(b, "-", sizeof(b));
	else
		fmt_mac(b, sizeof(b), x->factory_mac);
	vset(&eth_v, E_FMAC, b);
}

/* ==================================================================
 * Networks: WiFi (the connection, and the networks in range)
 * ================================================================== */
enum { W_STATE, W_SSID, W_BSSID, W_CHAN, W_RSSI, W_RATE, W_SEC, W_CHIP, W_FW, W_MAC, W_N };
static const char *const wifi_names[W_N] = { "State", "Network", "Access point", "Channel", "Signal", "Rate",
                                             "Security", "Chip", "Firmware", "MAC address" };
static struct vblock wifi_v;
static Object *nets_table;
static char nets_row[AN_SCAN_MAX][120];
static const char *nets_rows[AN_SCAN_MAX];
static int nets_n;

static Object *wifi_page(void)
{
	Object *g = vgroup(8);

	nets_table = mui_newc(table_mcc, MUIA_Table_Header, (ULONG)"Network\tSignal\tChannel\tSecurity", TAGEND);
	if (!nets_table)
		return NULL;
	mui_do(nets_table, MUIM_Notify, MUIA_Table_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_PICK);
	add(g, vblock_build(&wifi_v, wifi_names, W_N, 2));
	add(g, row(label("Networks in range", QL_TITLE), button("Sca_n", ID_SCAN), NULL, NULL));
	add(g, nets_table);
	return page_box(g);
}

static const char *find(const char *h, const char *n)
{
	size_t l = strlen(n);

	for (; *h; h++)
		if (!strncmp(h, n, l))
			return h;
	return NULL;
}

/* "wl0: Apr 15 2021 03:03:20 version 7.45.234 (4ca95bb CY) FWID ..." -> "7.45.234, Apr 15 2021" */
static void fmt_fw(char *b, int len, const volatile char *v)
{
	char t[96];
	const char *ver, *date;
	int i;

	for (i = 0; i < 95 && v[i]; i++)
		t[i] = v[i];
	t[i] = 0;
	ver = find(t, "version ");
	date = strchr(t, ':') ? strchr(t, ':') + 2 : t;
	if (!ver) {
		rt_strlcpy(b, t, (size_t)len);
		return;
	}
	ver += 8;
	{
		char vs[24], ds[16];
		int k = 0;
		while (ver[k] && ver[k] != ' ' && k < 23) { vs[k] = ver[k]; k++; }
		vs[k] = 0;
		k = 0;
		while (date[k] && k < 11) { ds[k] = date[k]; k++; }
		ds[k] = 0;
		rt_snprintf(b, (size_t)len, "%s, %s", vs, ds);
	}
}

static void wifi_refresh(void)
{
	volatile struct an_unit *x = U(0);
	static const char *const st[] = { "off", "starting", "ready", "scanning", "joining", "connected", "retrying",
	                                  "FAILED", "no firmware" };
	char b[VLEN];
	int conn = x->state == AN_ST_CONNECTED;

	if (x->kind == AN_KIND_NONE) {
		vset(&wifi_v, W_STATE, "not in use");
		return;
	}
	if (conn || !x->msg[0])
		rt_strlcpy(b, x->state < 9 ? st[x->state] : "?", sizeof(b));
	else
		rt_snprintf(b, sizeof(b), "%s: %s", x->state < 9 ? st[x->state] : "?", (const char *)x->msg);
	vset(&wifi_v, W_STATE, b);
	vset(&wifi_v, W_SSID, conn && x->ssid[0] ? (const char *)x->ssid : "-");
	if (conn)
		fmt_mac(b, sizeof(b), x->bssid);
	vset(&wifi_v, W_BSSID, conn ? b : "-");
	rt_snprintf(b, sizeof(b), "%lu", (unsigned long)x->channel);
	vset(&wifi_v, W_CHAN, conn ? b : "-");
	rt_snprintf(b, sizeof(b), "%ld dBm", (long)x->rssi);
	vset(&wifi_v, W_RSSI, conn ? b : "-");
	rt_snprintf(b, sizeof(b), "%lu Mbit/s", (unsigned long)x->speed_mbps);
	vset(&wifi_v, W_RATE, conn ? b : "-");
	vset(&wifi_v, W_SEC, conn ? secname(x->security) : "-");
	rt_snprintf(b, sizeof(b), "BCM%lx rev %lu, SDIO %lu MHz", (unsigned long)x->chip, (unsigned long)x->chiprev,
	            (unsigned long)x->sdmhz);
	vset(&wifi_v, W_CHIP, x->chip ? b : "-");
	if (x->fwver[0])
		fmt_fw(b, sizeof(b), x->fwver);
	vset(&wifi_v, W_FW, x->fwver[0] ? b : "-");
	fmt_mac_full(b, sizeof(b), x, "-");
	vset(&wifi_v, W_MAC, b);
}

static void nets_show(int n)
{
	int i, j;

	for (i = 1; i < n; i++)                         /* strongest first */
		for (j = i; j > 0 && scanbuf[j].we_Rssi > scanbuf[j - 1].we_Rssi; j--) {
			struct WifiScanEntry t = scanbuf[j];
			scanbuf[j] = scanbuf[j - 1];
			scanbuf[j - 1] = t;
		}
	for (i = 0; i < n; i++) {
		const struct WifiScanEntry *e = &scanbuf[i];
		rt_snprintf(nets_row[i], sizeof(nets_row[i]), "%s\n%ld dBm\t%u\t%s", e->we_Ssid[0] ? e->we_Ssid : "(hidden)",
		            (long)e->we_Rssi, (unsigned)e->we_Channel, secname(e->we_Security));
		nets_rows[i] = nets_row[i];
	}
	nets_n = n;
	mui_set(nets_table, MUIA_Table_NRows, (ULONG)nets_n);
	mui_set(nets_table, MUIA_Table_Rows, (ULONG)nets_rows);
}

/* ==================================================================
 * Settings: Ethernet
 * ================================================================== */
static Object *e_auto, *e_mode[6], *e_speed, *e_full, *e_pause;
static const char *const speeds[] = { "10 Mbit/s", "100 Mbit/s", "1000 Mbit/s", NULL };
static const char *const mode_names[6] = { "10 half", "10 full", "100 half", "100 full", "1000 half", "1000 full" };

static Object *ethset_page(void)
{
	Object *g = vgroup(6), *modes;
	int i;

	e_auto = checkbox(ID_AUTONEG);
	for (i = 0; i < 6; i++)
		e_mode[i] = checkbox(0);
	e_speed = mui_make(MUIO_Cycle, (ULONG)"", (ULONG)speeds);
	e_full = checkbox(0);
	e_pause = checkbox(0);
	if (!e_auto || !e_speed || !e_full || !e_pause)
		return NULL;
	mui_set(e_speed, MUIA_CycleChain, 1);
	modes = mui_new(MUIC_Group, MUIA_Group_Columns, 4, MUIA_Group_HorizSpacing, 6, MUIA_Group_VertSpacing, 4, TAGEND);
	for (i = 0; i < 6; i++) {
		add(modes, e_mode[i]);
		add(modes, label(mode_names[i], QL_LABEL));
	}
	add(modes, rect());
	add(modes, rect());
	add(g, left(label("Link", QL_TITLE)));
	add(g, check_row(e_auto, "Autonegotiate the speed and duplex"));
	add(g, left(label("Offer these speeds (Mbit/s) and duplexes:", QL_NOTE)));
	add(g, left(modes));
	add(g, left(label("Without autonegotiation, use:", QL_NOTE)));
	add(g, row(e_speed, e_full, label("full duplex", QL_LABEL), NULL));
	add(g, left(label("Flow control", QL_TITLE)));
	add(g, check_row(e_pause, "Offer and honour pause frames (full duplex)"));
	add(g, left(label("The port", QL_TITLE)));
	add(g, row(button("_Ethernet on", ID_ETH_ON), button("Ethernet off", ID_ETH_OFF), NULL, NULL));
	return page_box(g);
}

static void ethset_disable(void)
{
	int a = (int)mui_get(e_auto, MUIA_Selected), i;

	for (i = 0; i < 6; i++)
		mui_set(e_mode[i], MUIA_Disabled, !a);
	mui_set(e_speed, MUIA_Disabled, a);
	mui_set(e_full, MUIA_Disabled, a);
}

/* the gadgets from the settings now (the kernel's) */
static void ethset_load(ULONG adv, ULONG flags, ULONG speed)
{
	int i;

	mui_nnset(e_auto, MUIA_Selected, (flags & AN_EC_AUTONEG) != 0);
	for (i = 0; i < 6; i++)
		mui_nnset(e_mode[i], MUIA_Selected, (adv >> i) & 1);
	mui_nnset(e_speed, MUIA_Cycle_Active, speed == 10 ? 0 : speed == 100 ? 1 : 2);
	mui_nnset(e_full, MUIA_Selected, (flags & AN_EC_FULL) != 0);
	mui_nnset(e_pause, MUIA_Selected, (flags & AN_EC_PAUSE) != 0);
	ethset_disable();
}

static void ethset_read(struct NetEthCfg *c)
{
	int i, s = (int)mui_get(e_speed, MUIA_Cycle_Active);

	c->ec_Adv = 0;
	for (i = 0; i < 6; i++)
		if (mui_get(e_mode[i], MUIA_Selected))
			c->ec_Adv |= 1UL << i;
	c->ec_Flags = (mui_get(e_auto, MUIA_Selected) ? AN_EC_AUTONEG : 0) | (mui_get(e_full, MUIA_Selected) ? AN_EC_FULL : 0) |
	              (mui_get(e_pause, MUIA_Selected) ? AN_EC_PAUSE : 0);
	c->ec_Speed = s == 0 ? 10 : s == 1 ? 100 : 1000;
}

/* ==================================================================
 * Settings: WiFi
 * ================================================================== */
static Object *w_ssid, *w_pw_page, *w_pw[2], *w_show, *w_country, *w_ps;

static Object *wifiset_page(void)
{
	Object *g = vgroup(6), *form;

	w_ssid = string(33, 0);
	w_pw[0] = string(65, 1);
	w_pw[1] = string(65, 0);
	w_country = string(3, 0);
	w_show = checkbox(ID_SHOWPW);
	w_ps = checkbox(0);
	if (!w_ssid || !w_pw[0] || !w_pw[1] || !w_country || !w_show || !w_ps)
		return NULL;
	w_pw_page = mui_new(MUIC_Group, MUIA_Group_PageMode, TRUE, MUIA_Group_Child, (ULONG)w_pw[0],
	                    MUIA_Group_Child, (ULONG)w_pw[1], TAGEND);
	form = mui_new(MUIC_Group, MUIA_Group_Columns, 2, MUIA_Group_HorizSpacing, 8,
	               MUIA_Group_VertSpacing, theme_compact() ? 2 : 4,
	               MUIA_Group_Child, (ULONG)label("Network name", QL_LABEL), MUIA_Group_Child, (ULONG)w_ssid,
	               MUIA_Group_Child, (ULONG)label("Password", QL_LABEL),
	               MUIA_Group_Child, (ULONG)mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 6,
	                                                MUIA_Group_Child, (ULONG)w_pw_page,
	                                                MUIA_Group_Child, (ULONG)w_show,
	                                                MUIA_Group_Child, (ULONG)label("Show", QL_LABEL), TAGEND),
	               MUIA_Group_Child, (ULONG)label("Country", QL_LABEL),
	               MUIA_Group_Child, (ULONG)mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8,
	                                                MUIA_Group_Child, (ULONG)w_country,
	                                                MUIA_Group_Child, (ULONG)label("two letters: GB, AU, US, DE...", QL_NOTE),
	                                                TAGEND),
	               TAGEND);
	if (!w_pw_page || !form)
		return NULL;
	mui_set(w_country, MUIA_HorizWeight, 25);
	mui_set(w_pw_page, MUIA_ShortHelp, (ULONG)theme_bubble("Password",
	        "The network's passphrase, 8 to 63 characters (or 64 hex digits). Dots unless Show is ticked; "
	        "it is saved in ENVARC:alice68wifi.prefs, which anyone at the machine can read."));
	add(g, left(label("Network", QL_TITLE)));
	add(g, form);
	add(g, row(button("_Join and save", ID_JOIN), button("_Disconnect", ID_DISC), NULL, NULL));
	add(g, left(label("Radio", QL_TITLE)));
	add(g, check_row(w_ps, "Power saving (the chip dozes between beacons: slower to answer)"));
	add(g, row(button("_WiFi on", ID_WIFI_ON), button("WiFi off", ID_WIFI_OFF), NULL, NULL));
	return page_box(g);
}

/* ==================================================================
 * Traffic (one page per unit)
 * ================================================================== */
enum { T_RATE, T_PRATE, T_BYTES, T_PKTS, T_DROPS, T_ERRS, T_N };
static const char *const tr_names[T_N] = { "Rate", "Packet rate", "Bytes", "Packets", "Drops", "Errors" };
static struct vblock trv[2];
static Object *trgraph[2];

static Object *traffic_page(int u)
{
	Object *g = vgroup(8), *hdr;

	trgraph[u] = mui_newc(graph_mcc, MUIA_Graph_Hist, (ULONG)&trf[u].hist, MUIA_Graph_Kind, GK_BYTES,
	                      MUIA_Graph_Title, (ULONG)(u ? "Ethernet" : "WiFi"), MUIA_Graph_Lines, 7, TAGEND);
	hdr = mui_new(MUIC_Group, MUIA_Group_Columns, 3, MUIA_Group_HorizSpacing, 12,
	              MUIA_Group_Child, (ULONG)label("", QL_LABEL),
	              MUIA_Group_Child, (ULONG)label("In", QL_TITLE),
	              MUIA_Group_Child, (ULONG)label("Out", QL_TITLE), TAGEND);
	if (!trgraph[u] || !hdr)
		return NULL;
	add(g, trgraph[u]);
	add(g, hdr);
	add(g, vblock_build(&trv[u], tr_names, T_N, 3));
	return page_box(g);
}

static void traffic_refresh(int u)
{
	volatile struct an_unit *x = U(u);
	struct traffic *t = &trf[u];
	struct vblock *v = &trv[u];
	char b[VLEN];

	rp_fmt_rate(b, sizeof(b), t->rate_rxb); vset(v, T_RATE * 2, b);
	rp_fmt_rate(b, sizeof(b), t->rate_txb); vset(v, T_RATE * 2 + 1, b);
	rp_fmt_count(b, sizeof(b), t->rate_rxp); rt_strlcpy(b + strlen(b), " p/s", sizeof(b) - strlen(b)); vset(v, T_PRATE * 2, b);
	rp_fmt_count(b, sizeof(b), t->rate_txp); rt_strlcpy(b + strlen(b), " p/s", sizeof(b) - strlen(b)); vset(v, T_PRATE * 2 + 1, b);
	rp_fmt_bytes(b, sizeof(b), 0, x->rx_bytes); vset(v, T_BYTES * 2, b);
	rp_fmt_bytes(b, sizeof(b), 0, x->tx_bytes); vset(v, T_BYTES * 2 + 1, b);
	rp_fmt_count(b, sizeof(b), x->rx_pkts); vset(v, T_PKTS * 2, b);
	rp_fmt_count(b, sizeof(b), x->tx_pkts); vset(v, T_PKTS * 2 + 1, b);
	rp_fmt_count(b, sizeof(b), x->rx_drop); vset(v, T_DROPS * 2, b);
	vset(v, T_DROPS * 2 + 1, "-");
	rp_fmt_count(b, sizeof(b), u ? 0 : x->sd_errors); vset(v, T_ERRS * 2, b);
	rp_fmt_count(b, sizeof(b), x->tx_err); vset(v, T_ERRS * 2 + 1, b);
	mui_set(trgraph[u], MUIA_Graph_Hist, (ULONG)&trf[u].hist);
}

/* ==================================================================
 * the prefs files
 * ================================================================== */
static void file_read(const char *name, char *buf, LONG max)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	LONG n;

	buf[0] = 0;
	if (!fh)
		return;
	n = Read(fh, buf, max - 1);
	Close(fh);
	buf[n > 0 ? n : 0] = 0;
}

static int file_write(const char *name, const char *text)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
	LONG n = (LONG)strlen(text);
	int ok;

	if (!fh)
		return 0;
	ok = Write(fh, (APTR)text, n) == n;
	Close(fh);
	return ok;
}

/* KEY="value" or KEY=value; out "" when missing */
static void pref_get(const char *buf, const char *key, char *out, int max)
{
	int kl = (int)strlen(key), i = 0;
	const char *l = buf;

	out[0] = 0;
	while (*l) {
		while (*l == ' ' || *l == '\t')
			l++;
		if (!strncmp(l, key, (size_t)kl) && l[kl] == '=') {
			const char *v = l + kl + 1;
			int q = *v == '"';
			if (q)
				v++;
			while (*v && i < max - 1 && (q ? *v != '"' : (*v != ' ' && *v != '\n' && *v != '\r')))
				out[i++] = *v++;
			out[i] = 0;
			return;
		}
		while (*l && *l != '\n')
			l++;
		if (*l)
			l++;
	}
}

static void prefs_load(void)
{
	static char buf[1024];
	char v[80];

	file_read("ENV:alice68wifi.prefs", buf, sizeof(buf));
	if (!buf[0])
		file_read("ENVARC:alice68wifi.prefs", buf, sizeof(buf));
	pref_get(buf, "SSID", v, 34);
	mui_set(w_ssid, MUIA_String_Contents, (ULONG)v);
	pref_get(buf, "PASSWORD", v, 66);
	mui_set(w_pw[0], MUIA_String_Contents, (ULONG)v);
	mui_set(w_pw[1], MUIA_String_Contents, (ULONG)v);
	pref_get(buf, "COUNTRY", v, 3);
	mui_set(w_country, MUIA_String_Contents, (ULONG)v);
	/* the Ethernet's and the WiFi's settings: as the kernel has them now */
	if (nb) {
		volatile struct an_unit *e = U(1), *w = U(0);
		ethset_load(e->cfg_adv ? e->cfg_adv : 0x3f, e->cfg_adv ? e->cfg_flags : AN_EC_AUTONEG | AN_EC_FULL,
		            e->cfg_speed ? e->cfg_speed : 1000);
		mui_nnset(w_ps, MUIA_Selected, (w->cfg_flags & AN_WC_POWERSAVE) != 0);
	} else
		ethset_load(0x3f, AN_EC_AUTONEG | AN_EC_FULL, 1000);
}

static const char *pw_text(void)
{
	return (const char *)mui_get(w_pw[mui_get(w_show, MUIA_Selected) ? 1 : 0], MUIA_String_Contents);
}

/* 1: all four files written */
static int prefs_save(void)
{
	static char t[512];
	const char *ssid = (const char *)mui_get(w_ssid, MUIA_String_Contents);
	const char *cc = (const char *)mui_get(w_country, MUIA_String_Contents);
	struct NetEthCfg c;
	int ok = 1;

	rt_snprintf(t, sizeof(t), "SSID=\"%s\"\nPASSWORD=\"%s\"\n", ssid ? ssid : "", pw_text() ? pw_text() : "");
	if (cc && cc[0])
		rt_snprintf(t + strlen(t), sizeof(t) - strlen(t), "COUNTRY=%s\n", cc);
	ok &= file_write("ENV:alice68wifi.prefs", t);
	ok &= file_write("ENVARC:alice68wifi.prefs", t);
	ethset_read(&c);
	rt_snprintf(t, sizeof(t), "ETH=%lu,%lu,%lu\nWIFI=%lu\n", (unsigned long)c.ec_Adv, (unsigned long)c.ec_Flags,
	            (unsigned long)c.ec_Speed, (unsigned long)(mui_get(w_ps, MUIA_Selected) ? AN_WC_POWERSAVE : 0));
	ok &= file_write("ENV:alice68net.prefs", t);
	ok &= file_write("ENVARC:alice68net.prefs", t);
	return ok;
}

/* ==================================================================
 * actions
 * ================================================================== */
static void say(const char *s)
{
	if (s != note)
		rt_strlcpy(note, s, sizeof(note));
	if (status)
		mui_set(status, MUIA_Status_Text, (ULONG)note);
}

/* the Ethernet and WiFi settings to the kernel, when they differ from its own */
static int apply_settings(void)
{
	volatile struct an_unit *e = U(1), *w = U(0);
	ULONG ps = mui_get(w_ps, MUIA_Selected) ? AN_WC_POWERSAVE : 0;
	int sent = 0;

	ethset_read(&ecfg);
	if (ecfg.ec_Flags & AN_EC_AUTONEG && !ecfg.ec_Adv) {
		say("tick at least one speed to offer, or switch autonegotiation off");
		return -1;
	}
	if (!nb || ecfg.ec_Adv != e->cfg_adv || ecfg.ec_Flags != e->cfg_flags || ecfg.ec_Speed != e->cfg_speed)
		sent |= !dev_send(R_ECFG, NET_CMD_ETHCFG, &ecfg, sizeof(ecfg));
	if (!nb || ps != (w->cfg_flags & AN_WC_POWERSAVE)) {
		wcfg = ps;
		sent |= !dev_send(R_WCFG, NET_CMD_WIFICFG, &wcfg, sizeof(wcfg));
	}
	return sent;
}

static void do_join(int save)
{
	const char *ssid = (const char *)mui_get(w_ssid, MUIA_String_Contents);
	const char *pw = pw_text();
	const char *cc = (const char *)mui_get(w_country, MUIA_String_Contents);
	size_t pl = pw ? strlen(pw) : 0;

	if (!ssid || !ssid[0]) {
		say("type the network's name first (or click one on Networks: WiFi)");
		return;
	}
	if (pl && (pl < 8 || pl > 64)) {
		say("a password is 8 to 63 characters, or 64 hex digits");
		return;
	}
	memset(&joinbuf, 0, sizeof(joinbuf));
	rt_strlcpy(joinbuf.wj_Ssid, ssid, sizeof(joinbuf.wj_Ssid));
	rt_strlcpy(joinbuf.wj_Pass, pw ? pw : "", sizeof(joinbuf.wj_Pass));
	if (cc && cc[0] && cc[1]) {
		joinbuf.wj_Country[0] = (char)(cc[0] & ~0x20);
		joinbuf.wj_Country[1] = (char)(cc[1] & ~0x20);
	}
	if (dev_send(R_WIFI, WIFI_CMD_JOIN, &joinbuf, sizeof(joinbuf))) {
		say("the WiFi is busy: try again in a moment");
		return;
	}
	if (save)
		say(prefs_save() ? "joining; saved for the next start" : "joining; the prefs could not be saved");
	else
		say("joining (not saved: Save keeps it)");
}

static void show_password(int show)
{
	const char *t = (const char *)mui_get(w_pw[show ? 0 : 1], MUIA_String_Contents);
	static char keep[68];

	rt_strlcpy(keep, t ? t : "", sizeof(keep));
	mui_set(w_pw[show ? 1 : 0], MUIA_String_Contents, (ULONG)keep);
	mui_set(w_pw_page, MUIA_Group_ActivePage, (ULONG)(show ? 1 : 0));
}

/* a request came back */
static void dev_done(void)
{
	struct Message *m;
	int i;

	while ((m = GetMsg(ioport)) != NULL) {
		for (i = 0; i < NREQ; i++)
			if ((struct Message *)rq[i] == m)
				break;
		if (i == NREQ)
			continue;
		rbusy[i] = 0;
		if (i == R_SCAN && rcmd[i] == WIFI_CMD_SCAN) {
			if (rq[i]->io_Error)
				say("the scan did not finish");
			else {
				nets_show((int)rq[i]->io_Actual);
				rt_snprintf(note, sizeof(note), "%lu networks in range", (unsigned long)rq[i]->io_Actual);
				say(note);
			}
		} else if (rq[i]->io_Error)
			say(rcmd[i] == NET_CMD_ETHCFG ? "the Ethernet did not take those settings" : "the card said no");
	}
}

/* ==================================================================
 * once a second
 * ================================================================== */
static ULONG last_tick_us;

static void refresh(void)
{
	ULONG now, ms;

	if (!nb)
		return;
	now = ((volatile struct an_hdr *)nb)->boot_us;
	ms = last_tick_us ? (now - last_tick_us) / 1000u : 0;
	last_tick_us = now;
	traffic_sample(0, ms);
	traffic_sample(1, ms);
	rp_hist_push(&trf_all.hist, trf[0].rate_rxb + trf[1].rate_rxb, trf[0].rate_txb + trf[1].rate_txb);
	switch (page) {
	case P_STATUS: status_refresh(); break;
	case P_ETH: eth_refresh(); break;
	case P_WIFI: wifi_refresh(); break;
	case P_ETHTRAFFIC: traffic_refresh(1); break;
	case P_WIFITRAFFIC: traffic_refresh(0); break;
	}
}

/* ==================================================================
 * the window
 * ================================================================== */
static Object *build_page(int p)
{
	switch (p) {
	case P_ABOUT: return about_page();
	case P_STATUS: return status_page();
	case P_ETH: return eth_page();
	case P_WIFI: return wifi_page();
	case P_ETHSET: return ethset_page();
	case P_WIFISET: return wifiset_page();
	case P_ETHTRAFFIC: return traffic_page(1);
	case P_WIFITRAFFIC: return traffic_page(0);
	}
	return NULL;
}

static int build_window(void)
{
	static const char *const blabel[3] = { "_Save", "_Apply", "_Close" };
	static const ULONG bid[3] = { ID_SAVE, ID_APPLY, ID_CLOSE };
	static const char *const bhelp[3] = {
		"Applies the settings and keeps them, and the WiFi network, for the next start; joins the "
		"network typed in if the WiFi is not on it already.",
		"Applies the Ethernet and WiFi settings now, without keeping them, and joins the network "
		"typed in if the WiFi is not on it already.",
		"Closes the window. The card keeps running." };
	Object *right, *top;
	int i;

	nav = mui_newc(nav_mcc, MUIA_Nav_Active, (ULONG)page, MUIA_ShortHelp, (ULONG)" ", TAGEND);
	head = mui_newc(head_mcc, MUIA_Head_Page, (ULONG)page, TAGEND);
	pages = mui_new(MUIC_Group, MUIA_Group_PageMode, TRUE, TAGEND);
	status = mui_newc(status_mcc, TAGEND);
	bar = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_SameSize, TRUE, MUIA_Group_Spacing, 8,
	              MUIA_InnerLeft, 8, MUIA_InnerRight, 8, MUIA_InnerTop, theme_compact() ? 3 : 6,
	              MUIA_InnerBottom, theme_compact() ? 3 : 6, MUIA_Background, (ULONG)theme_spec(T_PANEL), TAGEND);
	if (!nav || !head || !pages || !status || !bar)
		return -1;
	for (i = 0; i < RP_NPAGES; i++) {
		Object *pg = build_page(i);
		if (!pg)
			return -1;
		add(pages, pg);
	}
	for (i = 0; i < 3; i++) {
		btn[i] = button(blabel[i], bid[i]);
		if (!btn[i])
			return -1;
		mui_set(btn[i], MUIA_ShortHelp, (ULONG)theme_bubble(blabel[i] + 1, bhelp[i]));
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
	                     (ULONG)bar, MUIA_Group_Child, (ULONG)status, TAGEND) : NULL;
	if (!root)
		return -1;
	win = mui_new(MUIC_Window, MUIA_Window_Title, (ULONG)A68N_TITLE, MUIA_Window_ID, 0x41363846UL /* A68N */,
	              MUIA_Window_RootObject, (ULONG)root, TAGEND);
	if (!win)
		return -1;
	add(app, win);
	mui_do(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (ULONG)app, 2, MUIM_Application_ReturnID, ID_CLOSE);
	mui_do(app, MUIM_Notify, MUIA_Application_DoubleStart, MUIV_EveryTime, (ULONG)app, 2,
	       MUIM_Application_ReturnID, ID_FRONT);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)pages, 3, MUIM_Set, MUIA_Group_ActivePage,
	       MUIV_TriggerValue);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)head, 3, MUIM_Set, MUIA_Head_Page,
	       MUIV_TriggerValue);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_PAGE);
	return 0;
}

static void go_page(int p)
{
	page = p;
	mui_set(nav, MUIA_Nav_Active, (ULONG)p);
	refresh();
}

/* 1: quit */
static int command(ULONG id)
{
	switch (id) {
	case ID_CLOSE:
		return 1;
	case ID_FRONT:
		mui_do(win, MUIM_Window_ToFront);
		break;
	case ID_PAGE:
		page = (int)mui_get(nav, MUIA_Nav_Active);
		refresh();
		break;
	case ID_SAVE:
	case ID_APPLY: {
		const char *ssid = (const char *)mui_get(w_ssid, MUIA_String_Contents);
		volatile struct an_unit *w = U(0);
		int r = apply_settings();
		if (r < 0)
			break;
		if (ssid && ssid[0] && nb && (w->state != AN_ST_CONNECTED || strcmp(ssid, (const char *)w->ssid)))
			do_join(id == ID_SAVE);
		else if (id == ID_SAVE)
			say(prefs_save() ? (r ? "applied, and saved for the next start" : "saved for the next start")
			                 : "the prefs could not be saved");
		else
			say(r ? "applied (not saved: Save keeps it)" : "nothing changed");
		break;
	}
	case ID_AUTONEG:
		ethset_disable();
		break;
	case ID_ETH_ON:
		say(dev_send(R_ETH, WIFI_CMD_START, NULL, 0) ? "busy" : "starting the Ethernet");
		break;
	case ID_ETH_OFF:
		say(dev_send(R_ETH, WIFI_CMD_OFF, NULL, 0) ? "busy" : "Ethernet off (a stack using it keeps it on)");
		break;
	case ID_WIFI_ON:
		say(dev_send(R_WIFI, WIFI_CMD_START, NULL, 0) ? "busy" : "starting the WiFi");
		break;
	case ID_WIFI_OFF:
		say(dev_send(R_WIFI, WIFI_CMD_OFF, NULL, 0) ? "busy" : "WiFi off (a stack using it keeps it on)");
		break;
	case ID_JOIN:
		do_join(1);
		break;
	case ID_DISC:
		say(dev_send(R_WIFI, WIFI_CMD_DISCONNECT, NULL, 0) ? "busy" : "leaving the network");
		break;
	case ID_SCAN:
		say(dev_send(R_SCAN, WIFI_CMD_SCAN, scanbuf, sizeof(scanbuf)) ? "a scan is running already" : "scanning (a few seconds)");
		break;
	case ID_PICK: {
		LONG r = (LONG)mui_get(nets_table, MUIA_Table_Active);
		if (r >= 0 && r < nets_n) {
			mui_set(w_ssid, MUIA_String_Contents, (ULONG)scanbuf[r].we_Ssid);
			go_page(P_WIFISET);
			mui_set(win, MUIA_Window_ActiveObject, (ULONG)w_pw[mui_get(w_show, MUIA_Selected) ? 1 : 0]);
			say("type the password and press Join and save");
		}
		break;
	}
	case ID_SHOWPW:
		show_password((int)mui_get(w_show, MUIA_Selected));
		break;
	}
	return 0;
}

/* the next second */
static void tick_send(struct timerequest *t)
{
	t->tr_node.io_Command = TR_ADDREQUEST;
	t->tr_time.tv_secs = 1;
	t->tr_time.tv_micro = 0;
	SendIO((struct IORequest *)t);
}

long amiga_main(char *args, long len)
{
	struct MsgPort *tport = NULL;
	struct timerequest *treq = NULL;
	int timer_on = 0, r = 20;
	ULONG sigs = 0;

	(void)args; (void)len;
	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
	MUIMasterBase = OpenLibrary((CONST_STRPTR)"muimaster.library", 19);
	if (DOSBase && !MUIMasterBase) {
		static const char t[] = "Alice68-Network needs MUI 3.8 or later (muimaster.library 19).\n"
		                        "Alice68-NetworkGT is a simpler window without MUI.\n";
		Write(Output(), (APTR)t, (LONG)sizeof(t) - 1);
	}
	if (!DOSBase || !IntuitionBase || !GfxBase || !MUIMasterBase)
		goto out;
	find_board();
	{
		struct Screen *s = LockPubScreen(NULL);
		if (s) {
			theme_set_compact(s->Height < 400);
			UnlockPubScreen(NULL, s);
		}
	}
	if (rp_classes_make() < 0 || widgets_classes_make() < 0)
		goto out;
	app = mui_new(MUIC_Application, MUIA_Application_Title, (ULONG)"Alice68-Network", MUIA_Application_Version,
	              (ULONG)vers, MUIA_Application_Copyright, (ULONG)"GPL-2.0", MUIA_Application_Author,
	              (ULONG)"The Alice68 and Riptide authors",
	              MUIA_Application_Description, (ULONG)"Alice68's WiFi and Ethernet",
	              MUIA_Application_Base, (ULONG)"ALICE68NET", MUIA_Application_UseCommodities, FALSE,
	              MUIA_Application_UseRexx, FALSE, MUIA_Application_SingleTask, TRUE, TAGEND);
	if (!app) {
		r = MUI_Error() == MUIE_SingleTask ? 0 : 20;
		goto out;
	}
	if (build_window() < 0)
		goto out;
	prefs_load();
	dev_open();
	mui_set(win, MUIA_Window_Open, TRUE);
	if (!mui_get(win, MUIA_Window_Open))
		goto out;
	if (!nb)
		say("no Alice68 network board: this is not an Alice68 kernel with the network");
	else if (!ropen[R_WIFI] && !ropen[R_ETH])
		say("alice68net.device could not be opened: is it in DEVS:?");
	else
		say("Alice68-Network " A68N_VERSION);
	tport = CreateMsgPort();
	treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest)) : NULL;
	if (treq && !OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)treq, 0))
		timer_on = 1;
	refresh();
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
			ULONG got = Wait(sigs | SIGBREAKF_CTRL_C | (ioport ? 1UL << ioport->mp_SigBit : 0) |
			                 (timer_on ? 1UL << tport->mp_SigBit : 0));
			if (got & SIGBREAKF_CTRL_C)
				break;
			if (ioport && (got & (1UL << ioport->mp_SigBit)))
				dev_done();
			if (timer_on && (got & (1UL << tport->mp_SigBit)) && CheckIO((struct IORequest *)treq)) {
				WaitIO((struct IORequest *)treq);
				refresh();
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
		mui_set(win, MUIA_Window_Open, FALSE);
		MUI_DisposeObject(app);
	}
	dev_close();
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
	return r;
}
