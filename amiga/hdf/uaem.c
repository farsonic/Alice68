/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 folders: FS-UAE-style .uaem sidecars and %xx names (uaem.h). */
#include "uaem.h"

static uint8_t up(uint8_t c) { return c >= 'a' && c <= 'z' ? (uint8_t)(c - 32) : c; }
static int hexval(uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int ieq(const char *a, const char *b) { while (*a && up((uint8_t)*a) == up((uint8_t)*b)) { a++; b++; } return !*a && !*b; }
static int ends(const char *s, uint32_t n, const char *x)
{
    uint32_t k = slen(x);
    if (n < k) return 0;
    for (uint32_t i = 0; i < k; i++) if (up((uint8_t)s[n - k + i]) != up((uint8_t)x[i])) return 0;
    return 1;
}

int uaem_hidden(const char *host)
{
    uint32_t n = slen(host);
    if (host[0] == '.' && host[1] == '_') return 1;          /* macOS AppleDouble */
    return ends(host, n, ".uaem") || ieq(host, ".DS_Store") || ieq(host, "_UAEFSDB.___");
}

static int evil(uint8_t c)
{
    return c < 32 || c == '%' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '/' || c == '|' || c == '<' || c == '>' || c == ':';
}
/* CON, AUX, PRN, NUL, COM1-9, LPT1-9, alone or before a dot */
static int device(const char *s, uint32_t n)
{
    static const char *const d3[] = { "CON", "AUX", "PRN", "NUL" };
    uint32_t base = 0;
    while (base < n && s[base] != '.') base++;
    if (base == 3) {
        for (int i = 0; i < 4; i++)
            if (up((uint8_t)s[0]) == (uint8_t)d3[i][0] && up((uint8_t)s[1]) == (uint8_t)d3[i][1] && up((uint8_t)s[2]) == (uint8_t)d3[i][2]) return 1;
    } else if (base == 4 && s[3] >= '1' && s[3] <= '9') {
        if ((up((uint8_t)s[0]) == 'C' && up((uint8_t)s[1]) == 'O' && up((uint8_t)s[2]) == 'M') ||
            (up((uint8_t)s[0]) == 'L' && up((uint8_t)s[1]) == 'P' && up((uint8_t)s[2]) == 'T')) return 1;
    }
    return 0;
}

int uaem_encode(const char *amiga, uint32_t len, char *host, uint32_t max)
{
    static const char hx[] = "0123456789abcdef";
    uint32_t k = 0;
    int dev = device(amiga, len), last_uaem = ends(amiga, len, ".uaem");
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)amiga[i];
        int esc = evil(c) || (dev && i == 2) || (i == len - 1 && (c == '.' || c == ' ' || last_uaem));
        if (esc) {
            if (k + 3 >= max) return -1;
            host[k++] = '%'; host[k++] = hx[c >> 4]; host[k++] = hx[c & 15];
        } else {
            if (k + 1 >= max) return -1;
            host[k++] = (char)c;
        }
    }
    host[k] = 0;
    return (int)k;
}

void uaem_decode(const char *host, char *out, uint32_t max)
{
    uint32_t k = 0;
    for (uint32_t i = 0; host[i] && k + 1 < max; ) {
        int a, b;
        if (host[i] == '%' && (a = hexval((uint8_t)host[i + 1])) >= 0 && (b = hexval((uint8_t)host[i + 2])) >= 0) {
            out[k++] = (char)(a * 16 + b);
            i += 3;
        } else out[k++] = host[i++];
    }
    out[k] = 0;
}

int uaem_sidecar(const char *host, char *out, uint32_t max)
{
    uint32_t n = slen(host);
    if (n + 6 > max) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = host[i];
    const char *x = ".uaem";
    for (uint32_t i = 0; i < 6; i++) out[n + i] = x[i];
    return (int)(n + 5);
}

/* ---- dates: AmigaDOS (days since 1978-01-01) and the calendar --------------------------- */
static int leap(uint32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static void civil(uint32_t days, uint32_t *y, uint32_t *m, uint32_t *d)
{
    uint32_t yy = 1978;
    while (days >= (leap(yy) ? 366u : 365u)) { days -= leap(yy) ? 366 : 365; yy++; }
    uint32_t mm = 0;
    for (;;) {
        uint32_t ml = mdays[mm] + (mm == 1 && leap(yy));
        if (days < ml) break;
        days -= ml; mm++;
    }
    *y = yy; *m = mm + 1; *d = days + 1;
}
static int amiga_days(uint32_t y, uint32_t m, uint32_t d, uint32_t *days)
{
    if (y < 1978 || y > 2200 || m < 1 || m > 12 || d < 1 || d > (uint32_t)(mdays[m - 1] + (m == 2 && leap(y)))) return -1;
    uint32_t n = 0;
    for (uint32_t yy = 1978; yy < y; yy++) n += leap(yy) ? 366 : 365;
    for (uint32_t mm = 1; mm < m; mm++) n += mdays[mm - 1] + (mm == 2 && leap(y));
    *days = n + d - 1;
    return 0;
}

static int num(const uint8_t *t, uint32_t n, uint32_t *at, uint32_t digits, uint32_t *v)
{
    *v = 0;
    for (uint32_t i = 0; i < digits; i++, (*at)++) {
        if (*at >= n || t[*at] < '0' || t[*at] > '9') return -1;
        *v = *v * 10 + (uint32_t)(t[*at] - '0');
    }
    return 0;
}
static int lit(const uint8_t *t, uint32_t n, uint32_t *at, char c) { if (*at >= n || t[*at] != (uint8_t)c) return -1; (*at)++; return 0; }

int uaem_parse(const uint8_t *t, uint32_t n, struct uaem *m)
{
    static const char flags[] = "hsparwed";
    uint32_t at = 0;
    if (n >= 3 && t[0] == 0xef && t[1] == 0xbb && t[2] == 0xbf) at = 3;     /* UTF-8 byte order mark */
    if (at + 8 > n) return -1;
    m->prot = 0;
    for (int i = 0; i < 8; i++, at++) {
        uint8_t c = t[at];
        int on;
        if (c == '-') on = 0;
        else if (c == (uint8_t)flags[i] || c == (uint8_t)(flags[i] - 32)) on = 1;
        else return -1;
        uint32_t bit = 1u << (7 - i);
        if (i < 4) { if (on) m->prot |= bit; }              /* h s p a: set when shown */
        else if (!on) m->prot |= bit;                       /* r w e d: set when NOT allowed */
    }
    uint32_t y, mo, d, h, mi, s, cs;
    if (lit(t, n, &at, ' ') || num(t, n, &at, 4, &y) || lit(t, n, &at, '-') || num(t, n, &at, 2, &mo) || lit(t, n, &at, '-') ||
        num(t, n, &at, 2, &d) || lit(t, n, &at, ' ') || num(t, n, &at, 2, &h) || lit(t, n, &at, ':') || num(t, n, &at, 2, &mi) ||
        lit(t, n, &at, ':') || num(t, n, &at, 2, &s) || lit(t, n, &at, '.') || num(t, n, &at, 2, &cs)) return -1;
    if (h > 23 || mi > 59 || s > 59 || amiga_days(y, mo, d, &m->days)) return -1;
    m->mins = h * 60 + mi;
    m->ticks = s * 50 + cs / 2;
    uint32_t k = 0;
    if (at < n && t[at] == ' ') at++;
    while (at < n && t[at] != '\n' && t[at] != '\r' && k < UAEM_COMMENT - 1) m->comment[k++] = (char)t[at++];
    m->comment[k] = 0;
    return 0;
}

static void put(char *o, uint32_t *k, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) { o[*k + (uint32_t)i] = (char)('0' + v % 10); v /= 10; }
    *k += (uint32_t)digits;
}

uint32_t uaem_format(const struct uaem *m, char *o)
{
    static const char flags[] = "hsparwed";
    uint32_t k = 0, y, mo, d;
    for (int i = 0; i < 8; i++) {
        uint32_t bit = 1u << (7 - i);
        int on = i < 4 ? (m->prot & bit) != 0 : !(m->prot & bit);
        o[k++] = on ? flags[i] : '-';
    }
    civil(m->days, &y, &mo, &d);
    o[k++] = ' ';
    put(o, &k, y, 4); o[k++] = '-'; put(o, &k, mo, 2); o[k++] = '-'; put(o, &k, d, 2); o[k++] = ' ';
    put(o, &k, m->mins / 60 % 24, 2); o[k++] = ':'; put(o, &k, m->mins % 60, 2); o[k++] = ':';
    put(o, &k, m->ticks / 50 % 60, 2); o[k++] = '.'; put(o, &k, m->ticks % 50 * 2, 2);
    o[k++] = ' ';
    for (uint32_t i = 0; m->comment[i] && i < UAEM_COMMENT - 1; i++) o[k++] = m->comment[i];
    o[k++] = '\n';
    return k;
}

/* ----rwed and ----r-e- (FAT's read-only), any archive bit (FAT has its own) */
int uaem_plain(uint32_t prot)
{
    uint32_t p = prot & ~0x10u;
    return p == 0 || p == 0x05;
}
