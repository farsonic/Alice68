/*
 * Alice68Prefs: what every page is built from (ui.c), the pages (prefs_fields.h) and what
 * each page module gives the window (prefs.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef A68P_UI_H
#define A68P_UI_H
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui_classes.h"
#include "prefs/mui_theme.h"
#include "prefs/mui_widgets.h"
#include "prefs/mui_ids.h"
#include "prefs/prefs_fields.h"

#define A68P_VERSION "1.0"
#define A68P_DATE    "17.11.2026"
#define A68P_TITLE   "Alice68 Preferences"

extern Object *app, *win;
extern int page;                                /* the page shown */

/* building blocks */
Object *rect(void);
Object *hgroup2(Object *a, Object *b);
Object *label(const char *text, int kind);
Object *left(Object *o);
Object *button(const char *text, ULONG id);    /* "_Name": the key; returns id */
Object *checkbox(ULONG id);
Object *check_row(Object *cb, const char *text);
Object *string(ULONG maxlen, int secret);
Object *string_id(ULONG maxlen, ULONG id);     /* returns id on Return */
Object *cycle(const char *const *entries, ULONG id);
Object *row(Object *a, Object *b, Object *c, Object *d);
Object *page_box(Object *c);                    /* at the top of the page */
Object *page_full(Object *c);                   /* filling it (tables) */
Object *vgroup(int spacing);
Object *hgroup(int spacing);
Object *table(const char *header, ULONG id, const char *empty);   /* rows of one line; id on a click */
void    add(Object *g, Object *o);
void    help(Object *o, const char *name, const char *body);      /* a bubble */
void    ui_free(void);                          /* the bubbles' memory, at the end */
void    cut(char *b, int max);
const char *str_of(Object *s);                  /* a string gadget's text, never NULL */

/* "name  value" rows */
#define VMAX 14
#define VLEN 64
struct vblock {
	int n;
	Object *obj[VMAX];
	char text[VMAX][VLEN], shown[VMAX][VLEN];
};
Object *vblock_build(struct vblock *v, const char *const *names, int n, int cols);
void vset(struct vblock *v, int k, const char *s);

/* a table's rows, kept here (the table class does not copy them) */
struct rows {
	int n, max, len;
	char *buf;                                  /* max rows of len bytes */
	const char **ptr;
	Object *obj;
};
int  rows_init(struct rows *r, int max, int len);
void rows_free(struct rows *r);
void rows_begin(struct rows *r);
char *rows_add(struct rows *r);                 /* NULL when full */
void rows_end(struct rows *r);                  /* to the table */

/* the status line, and questions */
void say(const char *s);
void sayf(const char *fmt, ...);
int  ask(const char *text, const char *buttons);   /* MUI_Request: 1 the first button ... 0 the last */

/* the page modules: each builds its pages, refreshes the one shown, takes its return IDs */
Object *net_page(int p);
void net_start(void);
void net_stop(void);
ULONG net_signals(void);
void net_signal(ULONG got);
void net_tick(int p);
int  net_command(ULONG id);
int  net_save(int keep);                        /* Save (keep 1) or Use: -1 refused, else what was done */
int  net_changed(void);
const char *net_hello(void);                    /* why the network pages show nothing, or NULL */

Object *card_page(int p);
void card_start(void);
void card_stop(void);
void card_tick(int p);
void card_shown(int p);
int  card_command(ULONG id);
int  card_changed(void);
int  card_save(void);                           /* cmdline.txt, config.txt: 1 written, 0 nothing, -1 failed */

Object *text_page(int p);                       /* Status, Joystick, Logs, Monitor: the tools' text */
void text_tick(int p);
void text_shown(int p);
int  text_command(ULONG id);
void text_stop(void);

/* return IDs of the pages (mui_ids.h's are below 2000) */
enum { ID_NET = 2000, ID_CARD = 2100, ID_BT = 2200, ID_TEXT = 2300, ID_RESTART = 2400 };
#endif
