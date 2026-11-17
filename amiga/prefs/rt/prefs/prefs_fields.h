/*
 * Alice68Prefs: the pages the MUI classes taken from RiptidePrefs show (the nav
 * list on the left, the head above each page). Riptide's own prefs_fields.h
 * describes riptide.conf; this one Alice68Prefs's pages (texts in prefs.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_FIELDS_H
#define RT_PREFS_FIELDS_H

/* page numbers; each page's icon is IC_ + its number (mui_theme.h) */
enum {
	P_ABOUT,
	P_STATUS, P_SETTINGS, P_TEXT,        /* System: the kernel's status page, cmdline.txt, the text files */
	P_CARD, P_FILES,                     /* Storage */
	P_NETSTATUS, P_ETH, P_WIFI,          /* Network: both units, each one */
	P_ETHSET, P_WIFISET,                 /* their settings */
	P_ETHTRAFFIC, P_WIFITRAFFIC,         /* their traffic */
	P_BT, P_JOY,                         /* Input */
	P_LOGS, P_MONITOR,                   /* Diagnostics */
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
