/*
 * Alice68-Network: the pages the MUI classes taken from RiptidePrefs show
 * (the nav list on the left, the head above each page). Riptide's own
 * prefs_fields.h describes riptide.conf; this one Alice68-Network's pages.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_FIELDS_H
#define RT_PREFS_FIELDS_H

/* page numbers: P_STATUS and P_ABOUT lead the list, then the Ethernet pages,
   then (from RP_FIRST_ADVANCED) the WiFi pages; each page's icon is IC_ + its number */
enum {
	P_ETH, P_ETHTRAFFIC,                 /* Ethernet */
	P_WIFI, P_WIFINETS, P_WIFITRAFFIC,   /* WiFi */
	P_STATUS, P_ABOUT,
	RP_NPAGES
};
#define RP_FIRST_ADVANCED P_WIFI

struct rp_page {
	const char *title;
	const char *help;
};
extern const struct rp_page rp_pages[RP_NPAGES];

#endif
