/*
 * Alice68Prefs: the Network pages - the settings and statistics of Alice68's network card
 * (alice68net.device: unit 0 the Pi 4's WiFi, unit 1 its Gigabit Ethernet, both run by the
 * kernel on CPU1). This was Alice68-Network 2.3.
 *
 * Status (both units, a minute of traffic); Ethernet and WiFi (what each is doing, the WiFi
 * networks in range); their settings (the Ethernet's speeds, duplex, autonegotiation and flow
 * control; the WiFi's network, password, country and power saving); their traffic. The numbers
 * come straight from the network board, once a second; actions go to alice68net.device without
 * waiting (the window never blocks).
 *
 * What it keeps: ENV:alice68wifi.prefs and ENVARC: (the WiFi network: SSID, PASSWORD, COUNTRY -
 * also read by "Alice68 WIFI RECONNECT") and ENV:alice68net.prefs and ENVARC: (ETH=, WIFI= and
 * UNITn=OFF, which alice68net.device applies when it starts).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ui.h"
#include "prefs/stat_util.h"
#include "gen/expansion_calls.h"
#include "alice_net.h"
#include "wifi_api.h"

struct Library *ExpansionBase;

#define SEC_OPEN  0x01                  /* the kernel's SEC_* (wifi/fmac.h) */
#define SEC_WEP   0x02
#define SEC_WPA   0x04
#define SEC_WPA2  0x08
#define SEC_WPA3  0x10
#define SEC_8021X 0x20

/* return IDs */
enum {
	ID_ETH_SW = ID_NET, ID_AUTONEG, ID_WIFI_SW,
	ID_JOIN, ID_DISC, ID_SCAN, ID_PICK, ID_SHOWPW
};
static char note[120];                  /* what the last action did, for the status line */

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

/* the MAC address */
static void fmt_mac_full(char *buf, int len, volatile struct an_unit *x, const char *none)
{
	if (mac_zero(x->mac)) {
		rt_strlcpy(buf, none, (size_t)len);
		return;
	}
	fmt_mac(buf, len, x->mac);
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









/* the on/off switch of a unit, and the line beside it that says what that means now */
static Object *sw_obj[2], *sw_text[2];
static char sw_shown[2][96];                    /* the line the text shows now */

static Object *switch_row(int u, ULONG id)
{
	sw_obj[u] = mui_newc(switch_mcc, MUIA_Switch_On, 1, TAGEND);
	sw_shown[u][0] = 0;
	sw_text[u] = mui_newc(stext_mcc, MUIA_SText_Text, (ULONG)"", MUIA_SText_Lines, 1, TAGEND);
	if (!sw_obj[u] || !sw_text[u])
		return NULL;
	if (app)
		mui_do(sw_obj[u], MUIM_Notify, MUIA_Switch_On, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, id);
	mui_set(sw_obj[u], MUIA_ShortHelp, (ULONG)theme_bubble(u ? "Ethernet" : "WiFi",
	        "Off switches the hardware off: TCP/IP stacks see the link go down and nothing is sent or received, "
	        "also while a stack has the unit open. It stays off until switched on here. Save keeps it for the next start."));
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 10, MUIA_Group_Child, (ULONG)sw_obj[u],
	               MUIA_Group_Child, (ULONG)sw_text[u], TAGEND);
}

/* the switches from the card (no notification: that would send the command again) */
static void switch_refresh(void)
{
	static char t[2][96];
	int u;

	for (u = 0; u < 2 && nb; u++) {
		volatile struct an_unit *x = U(u);
		int on = !x->admin_off, busy = rbusy[u ? R_ETH : R_WIFI];
		if (!sw_obj[u])
			continue;
		if (x->kind == AN_KIND_NONE) {
			rt_strlcpy(t[u], u ? "not in use: eth=off in cmdline.txt" : "no WiFi", sizeof(t[u]));
			mui_set(sw_obj[u], MUIA_Disabled, TRUE);
		} else {
			if (!busy && (int)mui_get(sw_obj[u], MUIA_Switch_On) != on)
				mui_nnset(sw_obj[u], MUIA_Switch_On, on);
			if (!on)
				rt_strlcpy(t[u], "Off: stacks see the link down", sizeof(t[u]));
			else if (x->link)
				rt_snprintf(t[u], sizeof(t[u]), "On: link up, %lu Mbit/s", (unsigned long)x->speed_mbps);
			else if (u && x->state == AN_ST_OFF)
				rt_strlcpy(t[u], "On: starts when a TCP/IP stack uses unit 1", sizeof(t[u]));
			else
				rt_snprintf(t[u], sizeof(t[u]), "On: %s", x->msg[0] ? (const char *)x->msg : "no link");
		}
		if (strcmp(t[u], sw_shown[u])) {
			rt_strlcpy(sw_shown[u], t[u], sizeof(sw_shown[u]));
			mui_set(sw_text[u], MUIA_SText_Text, (ULONG)sw_shown[u]);
		}
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

	nets_table = mui_newc(table_mcc, MUIA_Table_Header, (ULONG)"Network\tSignal\tChannel\tSecurity", MUIA_Table_OneLine, 1,
	                      MUIA_Table_Empty, (ULONG)"No networks yet: press Scan.", TAGEND);
	if (!nets_table)
		return NULL;
	mui_do(nets_table, MUIM_Notify, MUIA_Table_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_PICK);
	add(g, vblock_build(&wifi_v, wifi_names, W_N, 2));
	add(g, row(label("Networks in range", QL_TITLE), button("Sca_n", ID_SCAN), NULL, NULL));
	add(g, nets_table);
	return page_full(g);
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
		rt_snprintf(nets_row[i], sizeof(nets_row[i]), "%s\t%ld dBm\t%u\t%s", e->we_Ssid[0] ? e->we_Ssid : "(hidden)",
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
	add(g, left(label("Port", QL_TITLE)));
	add(g, switch_row(1, ID_ETH_SW));
	add(g, left(label("Link", QL_TITLE)));
	add(g, check_row(e_auto, "Autonegotiate the speed and duplex"));
	add(g, left(label("Offer these speeds (Mbit/s) and duplexes:", QL_NOTE)));
	add(g, left(modes));
	add(g, left(label("Without autonegotiation, use:", QL_NOTE)));
	add(g, row(e_speed, e_full, label("full duplex", QL_LABEL), NULL));
	add(g, left(label("Flow control", QL_TITLE)));
	add(g, check_row(e_pause, "Offer and honour pause frames (full duplex)"));
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
	add(g, left(label("Radio", QL_TITLE)));
	add(g, switch_row(0, ID_WIFI_SW));
	add(g, left(label("Network", QL_TITLE)));
	add(g, form);
	add(g, row(button("_Join and save", ID_JOIN), button("_Disconnect", ID_DISC), NULL, NULL));
	add(g, left(label("Power", QL_TITLE)));
	add(g, check_row(w_ps, "Power saving (the chip dozes between beacons: slower to answer)"));
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
	if (sw_obj[0] && !mui_get(sw_obj[0], MUIA_Switch_On))
		rt_strlcpy(t + strlen(t), "UNIT0=OFF\n", sizeof(t) - strlen(t));
	if (sw_obj[1] && !mui_get(sw_obj[1], MUIA_Switch_On))
		rt_strlcpy(t + strlen(t), "UNIT1=OFF\n", sizeof(t) - strlen(t));
	ok &= file_write("ENV:alice68net.prefs", t);
	ok &= file_write("ENVARC:alice68net.prefs", t);
	return ok;
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
		} else if (rq[i]->io_Error && rcmd[i] == NET_CMD_ENABLE)
			say(rq[i]->io_Error == IOERR_NOCMD ? "alice68net.device is too old for the switch: install 2.8 from ALICE68:"
			                                   : "the card did not switch");
		else if (rq[i]->io_Error)
			say(rcmd[i] == NET_CMD_ETHCFG ? "the Ethernet did not take those settings" : "the card said no");
	}
}

/* ==================================================================
 * once a second
 * ================================================================== */
static ULONG last_tick_us;

/* once a second, whichever page is shown: the traffic counts and the switches */
void net_tick(int p)
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
	switch_refresh();
	switch (p) {
	case P_NETSTATUS: status_refresh(); break;
	case P_ETH: eth_refresh(); break;
	case P_WIFI: wifi_refresh(); break;
	case P_ETHTRAFFIC: traffic_refresh(1); break;
	case P_WIFITRAFFIC: traffic_refresh(0); break;
	}
}

Object *net_page(int p)
{
	switch (p) {
	case P_NETSTATUS: return status_page();
	case P_ETH: return eth_page();
	case P_WIFI: return wifi_page();
	case P_ETHSET: return ethset_page();
	case P_WIFISET: return wifiset_page();
	case P_ETHTRAFFIC: return traffic_page(1);
	case P_WIFITRAFFIC: return traffic_page(0);
	}
	return NULL;
}

void net_start(void)
{
	find_board();
	prefs_load();
	dev_open();
}

void net_stop(void)
{
	dev_close();
}

ULONG net_signals(void)
{
	return ioport ? 1UL << ioport->mp_SigBit : 0;
}

void net_signal(ULONG got)
{
	if (ioport && (got & (1UL << ioport->mp_SigBit)))
		dev_done();
}

/* what the window's status line says first */
const char *net_hello(void)
{
	if (!nb)
		return "no Alice68 network board: this is not an Alice68 kernel with the network";
	if (!ropen[R_WIFI] && !ropen[R_ETH])
		return "alice68net.device could not be opened: is it in DEVS:?";
	return NULL;
}

/* the settings differ from the kernel's, or the WiFi network typed in is not the one it is on */
int net_changed(void)
{
	const char *ssid = str_of(w_ssid);
	volatile struct an_unit *e, *w;
	struct NetEthCfg c;

	if (!nb || !w_ssid)
		return 0;
	e = U(1);
	w = U(0);
	ethset_read(&c);
	if (c.ec_Adv != e->cfg_adv || c.ec_Flags != e->cfg_flags || c.ec_Speed != e->cfg_speed)
		return e->kind != AN_KIND_NONE;
	if ((mui_get(w_ps, MUIA_Selected) ? AN_WC_POWERSAVE : 0) != (w->cfg_flags & AN_WC_POWERSAVE))
		return 1;
	return ssid[0] && (w->state != AN_ST_CONNECTED || strcmp(ssid, (const char *)w->ssid));
}

/* Save (keep) or Use: the settings to the kernel; the network typed in joined */
int net_save(int keep)
{
	const char *ssid = str_of(w_ssid);
	volatile struct an_unit *w = U(0);
	int r = apply_settings();

	if (r < 0)
		return -1;
	if (ssid[0] && nb && (w->state != AN_ST_CONNECTED || strcmp(ssid, (const char *)w->ssid)))
		do_join(keep);
	else if (keep)
		say(prefs_save() ? (r ? "network: applied, and saved for the next start" : "network: saved for the next start")
		                 : "the network prefs could not be saved");
	else
		say(r ? "network: applied (not saved: Save keeps it)" : "network: nothing changed");
	return r;
}

/* the pages' own buttons; 0 when it is not theirs */
int net_command(ULONG id)
{
	switch (id) {
	case ID_AUTONEG:
		ethset_disable();
		break;
	case ID_ETH_SW:
	case ID_WIFI_SW: {
		int u = id == ID_ETH_SW, on = (int)mui_get(sw_obj[u], MUIA_Switch_On);
		const char *what = u ? "Ethernet" : "WiFi";
		if (dev_send(u ? R_ETH : R_WIFI, NET_CMD_ENABLE, NULL, (ULONG)on)) {
			mui_nnset(sw_obj[u], MUIA_Switch_On, !on);
			say("busy: try again in a moment");
		} else {
			rt_snprintf(note, sizeof(note), on ? "%s on (Save keeps it on)" : "%s off: stacks see the link down (Save keeps it off)", what);
			say(note);
		}
		break;
	}
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
			extern void go_page(int p);
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
	default:
		return 0;
	}
	return 1;
}
