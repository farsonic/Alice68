/*
 * Riptide - RiptidePrefs: the Status page's arithmetic and wording (see
 * prefs_status.h). No floating point and no 64-bit division: both cost a
 * library call on the 68020 and this runs once a second.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include "rt/rt_types.h"
#include "prefs/stat_util.h"

/* rt_ctl_iface.type and .flags (BSD IFF_* values) */
#define ST_IFT_LOOP     3
#define ST_IFF_UP       0x0001
#define ST_IFF_RUNNING  0x0040

/* ---- history ---- */
void rp_hist_clear(struct rp_hist *h)
{
	memset(h, 0, sizeof(*h));
}

void rp_hist_push(struct rp_hist *h, u32 a, u32 b)
{
	int i;

	if (h->n < RP_HIST) {
		i = (h->head + h->n) % RP_HIST;
		h->n++;
	} else {
		i = h->head;
		h->head = (h->head + 1) % RP_HIST;
	}
	h->v[0][i] = a;
	h->v[1][i] = b;
	h->pushes++;
}

u32 rp_hist_at(const struct rp_hist *h, int s, int i)
{
	if (i < 0 || i >= h->n || s < 0 || s > 1)
		return 0;
	return h->v[s][(h->head + i) % RP_HIST];
}

u32 rp_hist_max(const struct rp_hist *h, int s)
{
	u32 m = 0, v;
	int i;

	for (i = 0; i < h->n; i++) {
		if (s != 1 && (v = rp_hist_at(h, 0, i)) > m)
			m = v;
		if (s != 0 && (v = rp_hist_at(h, 1, i)) > m)
			m = v;
	}
	return m;
}

u32 rp_nice_scale(u32 v, u32 min)
{
	static const u32 step[3] = { 1, 2, 5 };
	u32 p = 1;

	if (v < min)
		v = min;
	if (v == 0)
		return 1;
	for (;;) {
		int k;
		for (k = 0; k < 3; k++)
			if (step[k] * p >= v)
				return step[k] * p;
		if (p > 0xffffffffUL / 50)
			return 0xffffffffUL;
		p *= 10;
	}
}

int rp_scale_y(u32 v, u32 scale, int h)
{
	if (scale == 0 || h <= 0)
		return 0;
	if (v >= scale)
		return h;
	/* v * h must fit 32 bits: drop the same low bits of both */
	while (v > 0xffffffffUL / (u32)h) {
		v >>= 1;
		scale >>= 1;
	}
	return scale ? (int)(v * (u32)h / scale) : 0;
}

/* ---- rates ---- */
u32 rp_rate(u32 prev, u32 cur, u32 ms)
{
	u32 d = cur - prev;

	if (ms == 0)
		return 0;
	if (d <= 0xffffffffUL / 1000u)
		return d * 1000u / ms;
	return d / ms * 1000u + d % ms * 1000u / ms;
}

u32 rp_cpu_permille(u32 idle0, u32 now0, u32 idle1, u32 now1)
{
	u32 dn = now1 - now0, di = idle1 - idle0, busy;

	if (dn == 0 || di >= dn)
		return 0;
	busy = dn - di;
	while (dn > 4000000u) {
		dn >>= 1;
		busy >>= 1;
	}
	return dn ? busy * 1000u / dn : 0;
}

/* ---- wording ---- */
/* v hundredths: "4.56", "45.6", "456" */
static void three_digits(char *buf, int len, u32 v100, const char *unit)
{
	if (v100 < 1000)
		rt_snprintf(buf, (size_t)len, "%lu.%02lu %s", (unsigned long)(v100 / 100), (unsigned long)(v100 % 100), unit);
	else if (v100 < 10000)
		rt_snprintf(buf, (size_t)len, "%lu.%lu %s", (unsigned long)(v100 / 100), (unsigned long)(v100 / 10 % 10),
		            unit);
	else
		rt_snprintf(buf, (size_t)len, "%lu %s", (unsigned long)(v100 / 100), unit);
}

void rp_fmt_bytes(char *buf, int len, u32 hi, u32 lo)
{
	static const char *const unit[5] = { "B", "KB", "MB", "GB", "TB" };
	u64 v = ((u64)hi << 32) | lo;
	int u = 0;

	if (v < 1024) {
		rt_snprintf(buf, (size_t)len, "%lu B", (unsigned long)lo);
		return;
	}
	while (u < 4 && (v >> (10 * (u + 1))) >= 1)
		u++;
	if ((v >> (10 * u)) >= 1024 && u < 4)
		u++;
	three_digits(buf, len, (u32)((v * 100) >> (10 * u)), unit[u]);
}

void rp_fmt_rate(char *buf, int len, u32 bps)
{
	char b[24];

	rp_fmt_bytes(b, sizeof(b), 0, bps);
	rt_snprintf(buf, (size_t)len, "%s/s", b);
}

const char *rp_plural(unsigned long n)
{
	return n == 1 ? "" : "s";
}

void rp_fmt_count(char *buf, int len, u32 v)
{
	char t[16];
	int n = 0, i, k = 0;

	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (i = n - 1; i >= 0 && k < len - 1; i--) {
		buf[k++] = t[i];
		if (i && i % 3 == 0 && k < len - 1)
			buf[k++] = ' ';
	}
	buf[k] = 0;
}

void rp_fmt_dur(char *buf, int len, u32 s)
{
	if (s < 60)
		rt_snprintf(buf, (size_t)len, "%lu s", (unsigned long)s);
	else if (s < 3600)
		rt_snprintf(buf, (size_t)len, "%lu min", (unsigned long)(s / 60));
	else if (s < 86400)
		rt_snprintf(buf, (size_t)len, "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
	else
		rt_snprintf(buf, (size_t)len, "%lu day%s %lu h", (unsigned long)(s / 86400), s < 2 * 86400 ? "" : "s",
		            (unsigned long)(s / 3600 % 24));
}

void rp_fmt_uptime(char *buf, int len, u32 s)
{
	u32 d = s / 86400;

	s %= 86400;
	if (d)
		rt_snprintf(buf, (size_t)len, "%lu day%s %lu:%02lu:%02lu", (unsigned long)d, d == 1 ? "" : "s",
		            (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
	else
		rt_snprintf(buf, (size_t)len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
		            (unsigned long)(s % 60));
}

void rp_fmt_offset(char *buf, int len, s32 ms)
{
	char sign = ms < 0 ? '-' : '+';
	u32 a = ms < 0 ? (u32)(-(ms + 1)) + 1u : (u32)ms;

	if (a < 10000)
		rt_snprintf(buf, (size_t)len, "%c%lu.%03lu s", sign, (unsigned long)(a / 1000), (unsigned long)(a % 1000));
	else
		rt_snprintf(buf, (size_t)len, "%c%lu s", sign, (unsigned long)(a / 1000));
}

void rp_fmt_ms(char *buf, int len, u32 us)
{
	char t[16];

	if (us < 10000) {
		rt_snprintf(buf, (size_t)len, "%lu.%lu ms", (unsigned long)(us / 1000), (unsigned long)(us / 100 % 10));
		return;
	}
	rp_fmt_count(t, sizeof(t), us / 1000);
	rt_snprintf(buf, (size_t)len, "%s ms", t);
}
