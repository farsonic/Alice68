/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68: the setup screen's changes applied to cmdline.txt (cmdedit.h). */
#include "cmdedit.h"

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* the next option: start and length; quotes keep spaces in */
static const char *next_tok(const char *c, uint32_t *len)
{
    while (*c && is_sp(*c)) c++;
    const char *t = c;
    int q = 0;
    while (*c && (q || !is_sp(*c))) { if (*c == '"') q = !q; c++; }
    *len = (uint32_t)(c - t);
    return *len ? t : 0;
}

/* the option's name: up to '=' (or all of it) */
static uint32_t key_len(const char *t, uint32_t n)
{
    uint32_t k = 0;
    while (k < n && t[k] != '=') k++;
    return k;
}

/* is the option t (n bytes) dropped by a "-NAME" line of e? */
static int dropped(const char *t, uint32_t n, const char *e)
{
    uint32_t k = key_len(t, n);
    while (*e) {
        const char *l = e;
        while (*e && *e != '\n') e++;
        uint32_t ln = (uint32_t)(e - l);
        if (*e) e++;
        if (ln < 2 || l[0] != '-' || ln - 1 != k) continue;
        uint32_t i = 0;
        while (i < k && l[1 + i] == t[i]) i++;
        if (i == k) return 1;
    }
    return 0;
}

int cmd_apply(const char *old, const char *edits, char *out, uint32_t max)
{
    uint32_t n = 0, len;
#define PUT(c) do { if (n + 2 >= max) return -1; out[n++] = (char)(c); } while (0)
    for (const char *c = old ? old : "", *t; (t = next_tok(c, &len)); c = t + len) {
        if (dropped(t, len, edits)) continue;
        if (n) PUT(' ');
        for (uint32_t i = 0; i < len; i++) PUT(t[i]);
    }
    for (const char *e = edits; *e; ) {
        const char *l = e;
        while (*e && *e != '\n') e++;
        uint32_t ln = (uint32_t)(e - l);
        if (*e) e++;
        if (ln < 2 || l[0] != '+') continue;
        if (n) PUT(' ');
        for (uint32_t i = 1; i < ln; i++) PUT(l[i] == '\r' || l[i] == '\t' ? ' ' : l[i]);
    }
    out[n++] = '\n';                    /* room kept by PUT */
    out[n] = 0;
    return (int)n;
#undef PUT
}
