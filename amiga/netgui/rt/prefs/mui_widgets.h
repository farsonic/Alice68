/*
 * Alice68-Network: the graph, tabs, table and stext classes (mui_widgets.c,
 * from Riptide's RiptidePrefs). Tags as Riptide's mui_status.c has them.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef A68_MUI_WIDGETS_H
#define A68_MUI_WIDGETS_H
#include "prefs/mui.h"
#include "prefs/mui_classes.h"

#define MUIA_Graph_Hist    (RTAG + 0x701)  /* (init, set) struct rp_hist *; set again to redraw */
#define MUIA_Graph_Kind    (RTAG + 0x702)  /* (init) GK_* */
#define MUIA_Graph_Title   (RTAG + 0x703)  /* (init) the name in the legend */
#define MUIA_Graph_Lines   (RTAG + 0x704)  /* (init) height in text lines */
#define MUIA_Graph_Ticker  (RTAG + 0x705)  /* (init) runs the page's timer */
#define MUIM_Graph_Tick    (RTAG + 0x780)  /* the timer */
#define MUIM_Graph_Retime  (RTAG + 0x781)  /* the timer again, at status_period() */
#define MUIA_Tabs_Labels   (RTAG + 0x801)  /* (init) const char *const *, NULL-ended */
#define MUIA_Tabs_Active   (RTAG + 0x802)  /* (init, set, get, notify) */
#define MUIA_Table_Rows    (RTAG + 0x901)  /* (set) const char *const *; set again to redraw */
#define MUIA_Table_NRows   (RTAG + 0x902)  /* (set, before Rows) */
#define MUIA_Table_Header  (RTAG + 0x903)  /* (init) the column titles, '\t' between */
#define MUIA_Table_Active  (RTAG + 0x904)  /* (set, get, notify) the chosen row, -1 none */
#define MUIA_IfView_Model  (RTAG + 0xa01)  /* (init) struct rp_status_model *; set Update to redraw */
#define MUIA_IfView_Update (RTAG + 0xa02)  /* (set) */
#define MUIA_SText_Text    (RTAG + 0xb01)  /* (init, set) lines separated by '\n' (not copied) */
#define MUIA_SText_Lines   (RTAG + 0xb02)  /* (init) the lines it has room for */
#define MUIA_Switch_On     (RTAG + 0xc01)  /* (init, set, get, notify) the switch: 1 on, 0 off */

enum { GK_BYTES, GK_CPU, GK_CONN };

extern struct MUI_CustomClass *graph_mcc, *tabs_mcc, *table_mcc, *stext_mcc, *switch_mcc;
int  widgets_classes_make(void);
void widgets_classes_free(void);
/* called on every tick of a graph made with MUIA_Graph_Ticker (once a second) */
void widgets_set_tick(void (*fn)(void));
#endif
