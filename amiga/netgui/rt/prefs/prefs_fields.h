/*
 * Alice68-Network: the pages the MUI classes taken from RiptidePrefs show
 * (the nav list on the left, the head above each page). Riptide's own
 * prefs_fields.h describes riptide.conf; this one Alice68-Network's pages.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_FIELDS_H
#define RT_PREFS_FIELDS_H

/* page numbers; each page's icon is IC_ + its number (mui_theme.h) */
enum {
	P_ABOUT, P_STATUS,
	P_ETH, P_WIFI,                       /* Networks: what each is doing */
	P_ETHSET, P_WIFISET,                 /* Settings */
	P_ETHTRAFFIC, P_WIFITRAFFIC,         /* Traffic */
	RP_NPAGES
};

struct rp_page {
	const char *title;
	const char *help;
};
extern const struct rp_page rp_pages[RP_NPAGES];
/* the list, top to bottom: the page on each row, and a heading above a row (or NULL) */
extern const int rp_nav_order[RP_NPAGES];
extern const char *const rp_nav_head[RP_NPAGES];

#endif
