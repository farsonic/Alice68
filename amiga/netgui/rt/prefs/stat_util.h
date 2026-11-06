/*
 * Alice68-Network: graph histories and number wording, from Riptide's
 * RiptidePrefs (its prefs_status.h/.c, the parts before the IP addresses).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef A68_STAT_UTIL_H
#define A68_STAT_UTIL_H
#include "rt/rt_types.h"

/* ---- the history of a graph: the last RP_HIST samples of two series ---- */
#define RP_HIST 60
struct rp_hist {
	u32 v[2][RP_HIST];      /* ring: the newest at (head + n - 1) % RP_HIST */
	int head, n;            /* oldest sample; samples held (up to RP_HIST) */
	u32 pushes;             /* samples ever pushed (a graph draws only what is new) */
};
void rp_hist_clear(struct rp_hist *h);
void rp_hist_push(struct rp_hist *h, u32 a, u32 b);
/* sample i (0 the oldest of the n held) of series s */
u32  rp_hist_at(const struct rp_hist *h, int s, int i);
/* the largest sample of series s (-1: both) */
u32  rp_hist_max(const struct rp_hist *h, int s);

/* The top of a graph's scale: the smallest 1, 2 or 5 times a power of
 * ten that is at least v (and at least min). */
u32  rp_nice_scale(u32 v, u32 min);
/* y of value v in a plot of height h pixels with that scale: 0 at the
 * bottom, h at the top (clamped) */
int  rp_scale_y(u32 v, u32 scale, int h);

/* ---- rates ---- */
/* What a counter that may wrap at 2^32 grew per second over ms
 * milliseconds (0 when ms is 0). No 64-bit division. */
u32  rp_rate(u32 prev, u32 cur, u32 ms);
/* The share of the time (0..1000, per mille) the Riptide process was
 * busy between two reads of net.riptide.status: idle and now ticks. */
u32  rp_cpu_permille(u32 idle0, u32 now0, u32 idle1, u32 now1);

/* ---- wording ---- */
/* "512 B", "12.3 KB", "4.56 MB", "1.20 GB" (KB = 1024 bytes) */
void rp_fmt_bytes(char *buf, int len, u32 hi, u32 lo);
/* "0 B/s", "812 B/s", "12.3 KB/s", "1.20 MB/s" */
void rp_fmt_rate(char *buf, int len, u32 bytes_per_s);
/* a count with thin groups: "1 234 567" */
void rp_fmt_count(char *buf, int len, u32 v);
/* "s" after a count other than 1 ("1 packet", "2 packets"), else "" */
const char *rp_plural(unsigned long n);
/* a duration: "45 s", "12 min", "3 h 05 min", "2 days 4 h" */
void rp_fmt_dur(char *buf, int len, u32 s);
/* an uptime: "0:00:45", "3:04:05", "2 days 3:04:05" */
void rp_fmt_uptime(char *buf, int len, u32 s);
/* milliseconds with a sign: "+0.012 s", "-1.250 s", "+12 s" (whole
 * seconds from 10 s up) */
void rp_fmt_offset(char *buf, int len, s32 ms);
/* microseconds as milliseconds: "0.4 ms", "12 ms", "1 200 ms" */
void rp_fmt_ms(char *buf, int len, u32 us);
#endif
