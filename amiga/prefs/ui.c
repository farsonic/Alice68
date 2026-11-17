/*
 * Alice68Prefs: the building blocks of its pages (ui.h), from Alice68-Network's.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdarg.h>
#include "ui.h"

extern Object *status_line;

void cut(char *b, int max)
{
	if ((int)strlen(b) > max && max > 3) {
		b[max - 3] = '.';
		b[max - 2] = '.';
		b[max - 1] = '.';
		b[max] = 0;
	}
}

Object *rect(void)
{
	return mui_new(MUIC_Rectangle, TAGEND);
}

Object *hgroup2(Object *a, Object *b)
{
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)a,
	               MUIA_Group_Child, (ULONG)b, TAGEND);
}

Object *label(const char *text, int kind)
{
	return mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)text, MUIA_QLabel_Kind, (ULONG)kind, TAGEND);
}

Object *left(Object *o)
{
	return o ? hgroup2(o, rect()) : NULL;
}

static void notify(Object *o, ULONG attr, ULONG val, ULONG id)
{
	if (o && app && id)
		mui_do(o, MUIM_Notify, attr, val, (ULONG)app, 2, MUIM_Application_ReturnID, id);
}

Object *button(const char *text, ULONG id)
{
	const char *u = strchr(text, '_');
	char key = u && u[1] ? (char)(u[1] >= 'A' && u[1] <= 'Z' ? u[1] + 32 : u[1]) : 0;
	Object *b = mui_newc(qbutton_mcc, MUIA_QButton_Label, (ULONG)text, MUIA_InputMode, MUIV_InputMode_RelVerify,
	                     MUIA_ControlChar, (ULONG)key, MUIA_CycleChain, 1, TAGEND);

	notify(b, MUIA_Pressed, FALSE, id);
	return b;
}

Object *checkbox(ULONG id)
{
	Object *g = mui_new(MUIC_Image, MUIA_Frame, MUIV_Frame_ImageButton, MUIA_InputMode, MUIV_InputMode_Toggle,
	                    MUIA_Image_Spec, MUII_CheckMark, MUIA_Image_FreeVert, FALSE, MUIA_Background, MUII_ButtonBack,
	                    MUIA_ShowSelState, FALSE, MUIA_CycleChain, 1, TAGEND);

	notify(g, MUIA_Selected, MUIV_EveryTime, id);
	return g;
}

/* a check box with its text right of it, the rest of the row empty */
Object *check_row(Object *cb, const char *text)
{
	return cb ? mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 6, MUIA_Group_Child, (ULONG)cb,
	                    MUIA_Group_Child, (ULONG)label(text, QL_LABEL), MUIA_Group_Child, (ULONG)rect(), TAGEND)
	          : NULL;
}

Object *string(ULONG maxlen, int secret)
{
	return mui_new(MUIC_String, MUIA_Frame, MUIV_Frame_String, MUIA_String_MaxLen, maxlen,
	               secret ? MUIA_String_Secret : TAG_IGNORE, TRUE, MUIA_CycleChain, 1, TAGEND);
}

Object *string_id(ULONG maxlen, ULONG id)
{
	Object *s = string(maxlen, 0);
	notify(s, MUIA_String_Acknowledge, MUIV_EveryTime, id);
	return s;
}

const char *str_of(Object *s)
{
	const char *t = s ? (const char *)mui_get(s, MUIA_String_Contents) : NULL;
	return t ? t : "";
}

Object *cycle(const char *const *entries, ULONG id)
{
	Object *c = mui_new(MUIC_Cycle, MUIA_Cycle_Entries, (ULONG)entries, MUIA_CycleChain, 1, TAGEND);
	notify(c, MUIA_Cycle_Active, MUIV_EveryTime, id);
	return c;
}

Object *row(Object *a, Object *b, Object *c, Object *d)
{
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8, MUIA_Group_Child, (ULONG)a,
	               b ? MUIA_Group_Child : TAG_IGNORE, (ULONG)b, c ? MUIA_Group_Child : TAG_IGNORE, (ULONG)c,
	               d ? MUIA_Group_Child : TAG_IGNORE, (ULONG)d, MUIA_Group_Child, (ULONG)rect(), TAGEND);
}

/* a page's contents with the theme's margins, at the top (page_box) or filling it (page_full) */
static Object *pbox(Object *c, int fill)
{
	ULONG in = theme_compact() ? 4 : 10;

	return c ? mui_new(MUIC_Group, MUIA_InnerLeft, 12, MUIA_InnerRight, 12, MUIA_InnerTop, in, MUIA_InnerBottom, in,
	                   MUIA_Group_VertSpacing, theme_compact() ? 3 : 6, MUIA_Group_Child, (ULONG)c,
	                   fill ? TAG_IGNORE : MUIA_Group_Child, (ULONG)rect(), TAGEND) : NULL;
}
Object *page_box(Object *c) { return pbox(c, 0); }
Object *page_full(Object *c) { return pbox(c, 1); }

Object *vgroup(int spacing)
{
	return mui_new(MUIC_Group, MUIA_Group_VertSpacing, theme_compact() ? spacing / 2 : spacing, TAGEND);
}

Object *hgroup(int spacing)
{
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_HorizSpacing, spacing, TAGEND);
}

Object *table(const char *header, ULONG id, const char *empty)
{
	Object *t = mui_newc(table_mcc, MUIA_Table_Header, (ULONG)header, MUIA_Table_OneLine, 1, MUIA_Table_Empty,
	                     (ULONG)(empty ? empty : "Nothing here."), TAGEND);
	notify(t, MUIA_Table_Active, MUIV_EveryTime, id);
	return t;
}

void add(Object *g, Object *o)
{
	if (g && o)
		mui_do(g, OM_ADDMEMBER, (ULONG)o);
}

static char *bubbles[128];
static int nbubbles;

void help(Object *o, const char *name, const char *body)
{
	char *b;

	if (!o || nbubbles >= 128 || !(b = AllocVec(400, MEMF_ANY)))
		return;
	theme_bubble_to(b, 400, name, body);
	bubbles[nbubbles++] = b;
	mui_set(o, MUIA_ShortHelp, (ULONG)b);
}

void ui_free(void)
{
	while (nbubbles)
		FreeVec(bubbles[--nbubbles]);
}

/* A block of "name  value" rows: names right-aligned in the first column, the values
 * (which change) in the others. */
Object *vblock_build(struct vblock *v, const char *const *names, int n, int cols)
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
void vset(struct vblock *v, int k, const char *s)
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

/* ---- table rows ---- */
int rows_init(struct rows *r, int max, int len)
{
	r->n = 0;
	r->max = max;
	r->len = len;
	r->buf = AllocVec((ULONG)max * (ULONG)len, MEMF_ANY | MEMF_CLEAR);
	r->ptr = AllocVec((ULONG)max * sizeof(char *), MEMF_ANY | MEMF_CLEAR);
	return r->buf && r->ptr ? 0 : -1;
}

void rows_free(struct rows *r)
{
	if (r->buf)
		FreeVec(r->buf);
	if (r->ptr)
		FreeVec(r->ptr);
	r->buf = NULL;
	r->ptr = NULL;
}

void rows_begin(struct rows *r)
{
	r->n = 0;
}

char *rows_add(struct rows *r)
{
	char *p;

	if (!r->buf || r->n >= r->max)
		return NULL;
	p = r->buf + (ULONG)r->n * (ULONG)r->len;
	p[0] = 0;
	r->ptr[r->n++] = p;
	return p;
}

void rows_end(struct rows *r)
{
	if (!r->obj)
		return;
	mui_set(r->obj, MUIA_Table_NRows, (ULONG)r->n);
	mui_set(r->obj, MUIA_Table_Rows, (ULONG)(r->n ? r->ptr : NULL));
}

/* ---- the status line, and questions ---- */
static char note[160];

void say(const char *s)
{
	if (s != note)
		rt_strlcpy(note, s, sizeof(note));
	if (status_line)
		mui_set(status_line, MUIA_Status_Text, (ULONG)note);
}

void sayf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(note, sizeof(note), fmt, ap);
	va_end(ap);
	say(note);
}

int ask(const char *text, const char *buttons)
{
	return (int)MUI_RequestA(app, win, 0, (CONST_STRPTR)A68P_TITLE, (CONST_STRPTR)buttons, (CONST_STRPTR)text, NULL);
}
