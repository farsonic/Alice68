/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 hardfiles - see hdf_core.h. */
#include "hdf_core.h"

#define FAT_WIN 64u                     /* FAT sectors read at a time (32 KB) */

/* ---- tiny formatter: %s %d %ld %u %lu %x %lx %c, width and 0 for numbers --- */
void hdf_fmt(char *out, unsigned size, const char *fmt, va_list ap)
{
    unsigned n = 0;
#define PUT(c) do { if (n + 1 < size) out[n++] = (char)(c); } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        char pad = ' ';
        unsigned w = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') w = w * 10 + (unsigned)(*fmt++ - '0');
        if (*fmt == 'l') fmt++;
        char tmp[12];
        unsigned k = 0;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) PUT(*s++);
            continue;
        } else if (*fmt == 'c') {
            PUT(va_arg(ap, int));
            continue;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            uint32_t v = va_arg(ap, uint32_t);
            int neg = 0;
            if (*fmt == 'd' && (int32_t)v < 0) { neg = 1; v = (uint32_t)(-(int32_t)v); }
            unsigned base = *fmt == 'x' ? 16 : 10;
            do { tmp[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            if (neg) tmp[k++] = '-';
            while (k < w && k < sizeof tmp) tmp[k++] = pad;
            while (k) PUT(tmp[--k]);
            continue;
        } else if (*fmt == '%') {
            PUT('%');
            continue;
        } else if (!*fmt) {
            break;
        }
    }
    out[n] = 0;
#undef PUT
}

void hdf_log(struct hdf_io *io, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    hdf_fmt(line, sizeof line, fmt, ap);
    va_end(ap);
    if (io->log) io->log(io->ctx, line);
}

static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }

/* ---- FAT ------------------------------------------------------------------ */
static int fat_type_ok(uint8_t t) { return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b || t == 0x0c || t == 0x0e; }

static int bpb_ok(const uint8_t *s)
{
    return (s[0] == 0xeb || s[0] == 0xe9) && le16(s + 11) == 512 && s[13] && !(s[13] & (s[13] - 1)) &&
           le16(s + 14) && s[16] && s[510] == 0x55 && s[511] == 0xaa;
}

int fat_open(struct fat *f, struct hdf_io *io)
{
    zero(f, sizeof *f);
    f->io = io;
    f->buf_first = ~0u;
    f->sec = io->alloc(io->ctx, HDF_BLOCK);
    f->buf = io->alloc(io->ctx, FAT_WIN * HDF_BLOCK);
    if (!f->sec || !f->buf) { hdf_log(io, "out of memory"); fat_close(f); return -1; }
    uint8_t *s = f->sec;
    if (io->read(io->ctx, 0, 1, s)) { hdf_log(io, "cannot read the card's first sector"); fat_close(f); return -1; }
    if (s[510] != 0x55 || s[511] != 0xaa) { hdf_log(io, "no partition table on the card"); fat_close(f); return -1; }
    if (!bpb_ok(s)) {                                         /* an MBR: the first FAT partition */
        uint32_t start = 0;
        for (int i = 0; i < 4 && !start; i++) {
            const uint8_t *e = s + 446 + 16 * i;
            if (e[4] == 0xee) { hdf_log(io, "GPT card: not supported"); fat_close(f); return -1; }
            if (fat_type_ok(e[4]) && le32(e + 8)) start = le32(e + 8);
        }
        if (!start) { hdf_log(io, "no FAT partition on the card"); fat_close(f); return -1; }
        f->part_start = start;
        if (io->read(io->ctx, start, 1, s) || !bpb_ok(s)) { hdf_log(io, "FAT partition at %lu: no valid boot sector", start); fat_close(f); return -1; }
    }
    uint32_t rsvd = le16(s + 14), nfats = s[16], roots = le16(s + 17);
    uint32_t fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36);
    uint32_t total = le16(s + 19) ? le16(s + 19) : le32(s + 32);
    f->spc = s[13];
    f->root_secs = (roots * 32 + 511) / 512;
    f->fat_start = f->part_start + rsvd;
    uint32_t data_off = rsvd + nfats * fatsz + f->root_secs;
    if (!fatsz || total <= data_off) { hdf_log(io, "FAT: bad geometry"); fat_close(f); return -1; }
    f->data_start = f->part_start + data_off;
    f->clusters = (total - data_off) / f->spc;
    if (f->clusters < 4085) { hdf_log(io, "FAT12: not supported"); fat_close(f); return -1; }
    f->fat32 = f->clusters >= 65525;
    f->fat_secs = fatsz;
    f->nfats = nfats;
    if (f->fat32) {
        f->root_clus = le32(s + 44);
        if (le16(s + 48) && le16(s + 48) != 0xffff && le16(s + 48) < rsvd) f->fsinfo = f->part_start + le16(s + 48);
    }
    else f->root_start = f->part_start + rsvd + nfats * fatsz;
    hdf_log(io, "FAT%d partition at sector %lu, %lu clusters of %lu bytes",
            f->fat32 ? 32 : 16, f->part_start, f->clusters, f->spc * 512);
    return 0;
}

void fat_close(struct fat *f)
{
    if (f->sec) f->io->free(f->io->ctx, f->sec, HDF_BLOCK);
    if (f->buf) f->io->free(f->io->ctx, f->buf, FAT_WIN * HDF_BLOCK);
    f->sec = f->buf = 0;
}

/* the next cluster in a chain; 0 = end, bad or unreadable */
static uint32_t fat_next(struct fat *f, uint32_t c)
{
    uint32_t off = f->fat32 ? c * 4 : c * 2;
    uint32_t sec = f->fat_start + off / 512;
    if (f->buf_first == ~0u || sec < f->buf_first || sec >= f->buf_first + FAT_WIN) {
        if (f->io->read(f->io->ctx, sec, FAT_WIN, f->buf)) { f->buf_first = ~0u; return 0; }
        f->buf_first = sec;
    }
    const uint8_t *p = f->buf + (sec - f->buf_first) * 512 + off % 512;
    uint32_t n = f->fat32 ? (le32(p) & 0x0fffffffu) : le16(p);
    if (n < 2 || n >= f->clusters + 2) return 0;              /* free, end, bad or out of range */
    return n;
}

static uint32_t clus_sec(const struct fat *f, uint32_t c) { return f->data_start + (c - 2) * f->spc; }

static int add_extent(struct hdf_io *io, struct hdf_file *h, uint32_t file_blk, uint32_t sec, uint32_t n)
{
    if (h->next && h->ext[h->next - 1].card_sec + h->ext[h->next - 1].count == sec) {
        h->ext[h->next - 1].count += n;
        return 0;
    }
    if (h->next == h->cap) {
        uint32_t cap = h->cap ? h->cap * 2 : 16;
        struct hdf_extent *e = io->alloc(io->ctx, cap * sizeof *e);
        if (!e) return -1;
        if (h->ext) { copy(e, h->ext, h->next * sizeof *e); io->free(io->ctx, h->ext, h->cap * sizeof *e); }
        h->ext = e;
        h->cap = cap;
    }
    h->ext[h->next].file_blk = file_blk;
    h->ext[h->next].card_sec = sec;
    h->ext[h->next].count = n;
    h->next++;
    return 0;
}

static int fat_extents(struct fat *f, uint32_t first, struct hdf_file *h)
{
    uint32_t need = h->blocks, have = 0, c = first, guard = f->clusters + 1;
    while (have < need) {
        if (c < 2 || !guard--) return -1;                     /* chain too short or looped */
        uint32_t n = need - have < f->spc ? need - have : f->spc;
        if (add_extent(f->io, h, have, clus_sec(f, c), n)) return -1;
        have += n;
        if (have < need) c = fat_next(f, c);
    }
    return 0;
}

void hdf_file_free(struct hdf_io *io, struct hdf_file *h)
{
    if (h->ext) io->free(io->ctx, h->ext, h->cap * sizeof *h->ext);
    h->ext = 0; h->cap = h->next = 0;
}

static uint8_t lfn_sum(const uint8_t *e)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* walk one directory; dir_clus 0 = the FAT16 root. found(): 1 = stop */
struct scan { struct hdf_file *files; int max, n; uint32_t hdf_dir; int wifi; };

static void take(struct fat *f, struct scan *sc, const uint8_t *e, const char *lname)
{
    if (sc->n >= sc->max) { hdf_log(f->io, "more than %d hardfiles: the rest are ignored", sc->max); return; }
    struct hdf_file *h = &sc->files[sc->n];
    zero(h, sizeof *h);
    unsigned k = 0;
    if (lname[0]) {
        for (; lname[k] && k < HDF_NAME - 1; k++) h->name[k] = lname[k];
    } else {
        /* byte 12: Windows' "lower-case base / extension" flags for 8.3 names */
        for (int i = 0; i < 8 && e[i] != ' '; i++) h->name[k++] = (char)((e[12] & 0x08) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
        h->name[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) h->name[k++] = (char)((e[12] & 0x10) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
    }
    h->name[k] = 0;
    h->size = le32(e + 28);
    h->blocks = h->size / HDF_BLOCK;
    uint32_t first = le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0);
    if (h->size % HDF_BLOCK) hdf_log(f->io, "%s: size %lu is not a multiple of 512 - the tail is ignored", h->name, h->size);
    if (h->blocks < 2) { hdf_log(f->io, "%s: too small, ignored", h->name); return; }
    if (fat_extents(f, first, h)) {
        hdf_log(f->io, "%s: cluster chain shorter than the file - ignored (run a disk check on the card)", h->name);
        hdf_file_free(f->io, h);
        return;
    }
    hdf_log(f->io, "%s: %lu MB, %lu piece%s on the card", h->name, h->size >> 20, h->next, h->next == 1 ? "" : "s");
    sc->n++;
}

/* the WiFi firmware's files: anything in the WIFI folder; in the root, the
   names the kernel looks for (cyfmac4345x..., brcmfmac4345x...) */
static int fw_name(const char *n)
{
    static const char *const pre[] = { "cyfmac4345", "brcmfmac4345", 0 };
    for (int i = 0; pre[i]; i++) {
        int k = 0;
        while (pre[i][k] && (n[k] == pre[i][k] || (n[k] >= 'A' && n[k] <= 'Z' && n[k] + 32 == pre[i][k]))) k++;
        if (!pre[i][k]) return 1;
    }
    return 0;
}
static void take_any(struct fat *f, struct scan *sc, const uint8_t *e, const char *lname, int top)
{
    if (sc->n >= sc->max) return;
    struct hdf_file *h = &sc->files[sc->n];
    zero(h, sizeof *h);
    unsigned k = 0;
    if (lname[0]) {
        for (; lname[k] && k < HDF_NAME - 1; k++) h->name[k] = lname[k];
    } else {
        for (int i = 0; i < 8 && e[i] != ' '; i++) h->name[k++] = (char)((e[12] & 0x08) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
        if (e[8] != ' ') h->name[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) h->name[k++] = (char)((e[12] & 0x10) && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
    }
    h->name[k] = 0;
    if (h->name[0] == '.' || (top && !fw_name(h->name))) return;   /* "._x" (macOS), other root files */
    h->size = le32(e + 28);
    if (!h->size || h->size > 4u * 1024 * 1024) return;
    h->blocks = (h->size + HDF_BLOCK - 1) / HDF_BLOCK;
    uint32_t first = le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0);
    if (fat_extents(f, first, h)) { hdf_log(f->io, "%s: cluster chain shorter than the file - ignored", h->name); hdf_file_free(f->io, h); return; }
    sc->n++;
}

static void scan_dir(struct fat *f, struct scan *sc, uint32_t dir_clus, int top)
{
    char lname[HDF_NAME];
    uint8_t lsum = 0;
    lname[0] = 0;
    uint32_t c = dir_clus, idx = 0, guard = 65536;
    for (;;) {
        uint32_t sec;
        if (!dir_clus) {                                      /* FAT16 root: a fixed area */
            if (idx >= f->root_secs) return;
            sec = f->root_start + idx;
        } else {
            if (idx == f->spc) { c = fat_next(f, c); idx = 0; if (!c) return; }
            sec = clus_sec(f, c) + idx;
        }
        if (!guard--) return;
        idx++;
        if (f->io->read(f->io->ctx, sec, 1, f->sec)) { hdf_log(f->io, "directory read failed"); return; }
        for (int i = 0; i < 16; i++) {                        /* take() reads the FAT into f->buf only */
            const uint8_t *e = f->sec + 32 * i;
            if (e[0] == 0) return;
            if (e[0] == 0xe5) { lname[0] = 0; continue; }
            if (e[11] == 0x0f) {                              /* a long-name piece */
                unsigned seq = e[0] & 0x1f;
                if (e[0] & 0x40) { for (unsigned k = 0; k < HDF_NAME; k++) lname[k] = 0; lsum = e[13]; }
                if (!seq || seq > 5 || e[13] != lsum) { lname[0] = 0; continue; }
                static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (unsigned k = 0; k < 13; k++) {
                    unsigned pos = (seq - 1) * 13 + k;
                    uint32_t ch = le16(e + at[k]);
                    if (pos >= HDF_NAME - 1 || ch == 0 || ch == 0xffff) continue;
                    lname[pos] = (char)(ch < 0x80 ? ch : '?');
                }
                continue;
            }
            const char *ln = (lname[0] && lfn_sum(e) == lsum) ? lname : "";
            if (sc->wifi && !(e[11] & 0x08)) {
                if (e[11] & 0x10) {
                    if (top && !sc->hdf_dir && e[0] == 'W' && e[1] == 'I' && e[2] == 'F' && e[3] == 'I' && e[4] == ' ' && e[8] == ' ')
                        sc->hdf_dir = le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0);
                } else take_any(f, sc, e, ln, top);
            } else if (!(e[11] & 0x08)) {                     /* not a volume label */
                if (e[11] & 0x10) {
                    if (top && !sc->hdf_dir && e[0] == 'H' && e[1] == 'D' && e[2] == 'F' && e[3] == ' ' && e[8] == ' ')
                        sc->hdf_dir = le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0);
                } else if ((e[8] == 'H' || e[8] == 'h') && (e[9] == 'D' || e[9] == 'd') && (e[10] == 'F' || e[10] == 'f')) {
                    /* "._Workbench.hdf" and other dot files: macOS's metadata next to the real
                       file, never a hardfile. Their 8.3 names start with '_' and lose the dot,
                       so the long name tells */
                    if (ln[0] != '.') take(f, sc, e, ln);
                } else if (top) {                             /* cmdline.txt and config.txt */
                    static const char names[2][11] = { { 'C', 'M', 'D', 'L', 'I', 'N', 'E', ' ', 'T', 'X', 'T' },
                                                       { 'C', 'O', 'N', 'F', 'I', 'G', ' ', ' ', 'T', 'X', 'T' } };
                    for (int w = 0; w < 2; w++) {
                        int k = 0;
                        while (k < 11 && (e[k] == names[w][k] || (e[k] >= 'a' && e[k] <= 'z' && e[k] - 32 == names[w][k]))) k++;
                        uint32_t *clus = w ? &f->cfg_clus : &f->cmd_clus, *size = w ? &f->cfg_size : &f->cmd_size;
                        if (k == 11 && !*clus) {
                            *clus = le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0);
                            *size = le32(e + 28);
                        }
                    }
                }
            }
            lname[0] = 0;
        }
    }
}

int fat_find_wifi(struct fat *f, struct hdf_file *files, int max)
{
    struct scan sc = { files, max, 0, 0, 1 };
    scan_dir(f, &sc, f->fat32 ? f->root_clus : 0, 1);
    if (sc.hdf_dir) scan_dir(f, &sc, sc.hdf_dir, 0);
    return sc.n;
}

int fat_find_hdfs(struct fat *f, struct hdf_file *files, int max)
{
    struct scan sc = { files, max, 0, 0, 0 };
    scan_dir(f, &sc, f->fat32 ? f->root_clus : 0, 1);
    if (sc.hdf_dir) scan_dir(f, &sc, sc.hdf_dir, 0);
    return sc.n;
}

static int read_small(struct fat *f, uint32_t clus, uint32_t size, char *buf, uint32_t max)
{
    if (!max) return -1;
    buf[0] = 0;
    if (!clus || clus < 2) return -1;
    uint32_t n = size < max - 1 ? size : max - 1, got = 0, c = clus, guard = 256;
    while (got < n && c && guard--) {
        for (uint32_t i = 0; i < f->spc && got < n; i++) {
            if (f->io->read(f->io->ctx, clus_sec(f, c) + i, 1, f->sec)) { buf[got] = 0; return -1; }
            uint32_t k = n - got < 512 ? n - got : 512;
            copy(buf + got, f->sec, k);
            got += k;
        }
        if (got < n) c = fat_next(f, c);
    }
    buf[got] = 0;
    return (int)got;
}
int fat_read_cmdline(struct fat *f, char *buf, uint32_t max) { return read_small(f, f->cmd_clus, f->cmd_size, buf, max); }
int fat_read_config(struct fat *f, char *buf, uint32_t max) { return read_small(f, f->cfg_clus, f->cfg_size, buf, max); }

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int tok_eq(const char *t, unsigned n, const char *w)
{
    unsigned k = 0;
    while (k < n && w[k] && t[k] == w[k]) k++;
    return k == n && !w[k];
}

void hdf_parse_opts(const char *c, struct hdf_opts *o)
{
    zero(o, sizeof *o);
    while (c && *c) {
        while (is_space(*c)) c++;
        const char *t = c;
        while (*c && !is_space(*c)) c++;
        unsigned n = (unsigned)(c - t);
        if (tok_eq(t, n, "hdf=off")) o->off = 1;
        else if (tok_eq(t, n, "hdf.readonly") || tok_eq(t, n, "hdf.ro")) o->readonly = 1;
        else if (tok_eq(t, n, "hdf.noboot")) o->noboot = 1;
        else if (n > 9 && tok_eq(t, 9, "hdf.boot=")) {
            unsigned k = 0;
            for (; k + 9 < n && k < HDF_NAME - 1; k++) o->boot[k] = t[9 + k];
            o->boot[k] = 0;
        }
    }
}

int hdf_name_match(const char *a, const char *b)
{
    if (!a[0] || !b[0]) return 0;
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x == '_') x = ' ';
        if (y == '_') y = ' ';
        if (x != y) return 0;
    }
    return !*a && !*b;
}

/* ---- hardfile blocks -------------------------------------------------------- */
int hdf_map(const struct hdf_file *h, uint32_t blk, uint32_t *card_sec, uint32_t *run)
{
    if (blk >= h->blocks || !h->next) return -1;
    uint32_t lo = 0, hi = h->next - 1;
    while (lo < hi) {                                         /* the last extent starting at or before blk */
        uint32_t mid = (lo + hi + 1) / 2;
        if (h->ext[mid].file_blk <= blk) lo = mid; else hi = mid - 1;
    }
    const struct hdf_extent *e = &h->ext[lo];
    uint32_t off = blk - e->file_blk;
    if (off >= e->count) return -1;
    *card_sec = e->card_sec + off;
    *run = e->count - off;
    return 0;
}

int hdf_read(struct hdf_io *io, const struct hdf_file *h, uint32_t blk, uint32_t count, void *buf)
{
    uint8_t *p = buf;
    while (count) {
        uint32_t sec, run;
        if (hdf_map(h, blk, &sec, &run)) return -1;
        if (run > count) run = count;
        if (io->read(io->ctx, sec, run, p)) return -1;
        blk += run; count -= run; p += run * HDF_BLOCK;
    }
    return 0;
}

int hdf_write(struct hdf_io *io, const struct hdf_file *h, uint32_t blk, uint32_t count, const void *buf)
{
    const uint8_t *p = buf;
    if (!io->write) return -2;
    if (blk > h->blocks || count > h->blocks - blk) return -1;   /* all of it inside the file, or nothing */
    while (count) {
        uint32_t sec, run;
        if (hdf_map(h, blk, &sec, &run)) return -1;           /* never outside the file's own clusters */
        if (run > count) run = count;
        int r = io->write(io->ctx, sec, run, p);
        if (r) return r == -2 ? -2 : -1;
        blk += run; count -= run; p += run * HDF_BLOCK;
    }
    return 0;
}

/* ---- Rigid Disk Block ------------------------------------------------------- */
static int rdb_sum_ok(const uint8_t *b)
{
    uint32_t n = be32(b + 4), s = 0;
    if (n < 2 || n > 128) return 0;
    for (uint32_t i = 0; i < n; i++) s += be32(b + 4 * i);
    return s == 0;
}

int hdf_rdb(struct hdf_io *io, const struct hdf_file *h, struct hdf_rdb *r)
{
    uint8_t *b = io->alloc(io->ctx, HDF_BLOCK);
    if (!b) return -1;
    int ret = 0;
    zero(r, sizeof *r);
    r->fshd = ~0u;
    uint32_t at = ~0u;
    for (uint32_t i = 0; i < 16 && i < h->blocks; i++) {      /* RDB_LOCATION_LIMIT */
        if (hdf_read(io, h, i, 1, b)) { ret = -1; goto out; }
        if (be32(b) == 0x5244534bu && rdb_sum_ok(b)) { at = i; break; }   /* 'RDSK' */
    }
    if (at == ~0u) goto out;
    if (be32(b + 16) != HDF_BLOCK) { hdf_log(io, "%s: RDB block size %lu: only 512 is supported", h->name, be32(b + 16)); ret = -1; goto out; }
    r->cylinders = be32(b + 64); r->sectors = be32(b + 68); r->heads = be32(b + 72);
    r->fshd = be32(b + 32);
    uint32_t next = be32(b + 28), guard = 64;
    while (next != 0xffffffffu && guard--) {
        if (hdf_read(io, h, next, 1, b)) { ret = -1; goto out; }
        if (be32(b) != 0x50415254u || !rdb_sum_ok(b)) {       /* 'PART' */
            hdf_log(io, "%s: broken partition list at block %lu", h->name, next);
            ret = -1; goto out;
        }
        if (r->nparts < HDF_MAX_PARTS) {
            struct hdf_part *p = &r->part[r->nparts++];
            p->flags = be32(b + 20);
            unsigned len = b[36] < 31 ? b[36] : 31;           /* BSTR */
            for (unsigned k = 0; k < len; k++) p->name[k] = (char)b[37 + k];
            p->name[len] = 0;
            uint32_t ts = be32(b + 128);
            if (ts > 19) ts = 19;
            for (uint32_t k = 0; k <= ts; k++) p->envec[k] = be32(b + 128 + 4 * k);
            p->envec[0] = ts;
        }
        next = be32(b + 16);
    }
    ret = 1;
out:
    io->free(io->ctx, b, HDF_BLOCK);
    return ret;
}

/* ---- single-partition hardfiles ----------------------------------------------- */
/* WinUAE's hardfile geometry is 32 sectors, 1 surface, 2 reserved blocks;
   others are tried too. The right one puts FFS's root block - type 2 (T_SHORT),
   secondary type 1 (ST_ROOT) - at (reserved + last block) / 2. */
int hdf_single(struct hdf_io *io, const struct hdf_file *h, struct hdf_rdb *r)
{
    static const uint8_t geo[][3] = { { 1, 32, 2 }, { 1, 32, 1 }, { 4, 32, 2 }, { 16, 63, 2 }, { 1, 63, 2 } };
    uint8_t *b = io->alloc(io->ctx, HDF_BLOCK);
    if (!b) return 0;
    int ret = 0;
    if (hdf_read(io, h, 0, 1, b)) goto out;
    uint32_t dostype = be32(b);
    int letters = 1;
    for (int i = 0; i < 3; i++) if (b[i] < 'A' || b[i] > 'Z') letters = 0;
    /* "DOS\1", "PFS\3", "SFS\0": three letters and a version, never an RDB's "RDSK" */
    if (!letters || b[3] > 9 || dostype == 0x5244534bu) goto out;   /* not formatted, or not a filesystem */
    int ffs = (dostype & 0xffffff00u) == 0x444f5300u && (dostype & 0xff) <= 7;
    int pick = -1;
    for (unsigned g = 0; g < sizeof geo / sizeof geo[0]; g++) {
        uint32_t bpc = (uint32_t)geo[g][0] * geo[g][1], cyl = h->blocks / bpc;
        if (cyl < 2) continue;
        uint32_t root = (geo[g][2] + cyl * bpc - 1) / 2;
        if (!ffs) { pick = 0; break; }                        /* other filesystems: WinUAE's default */
        if (hdf_read(io, h, root, 1, b)) break;
        if (be32(b) == 2 && be32(b + 508) == 1) { pick = (int)g; break; }
    }
    if (pick < 0) {
        hdf_log(io, "%s: DOS type %c%c%c\\%lu, but no FFS root block where a known geometry puts it", h->name,
                (int)(dostype >> 24), (int)((dostype >> 16) & 0xff), (int)((dostype >> 8) & 0xff), dostype & 0xff);
        goto out;
    }
    zero(r, sizeof *r);
    r->fshd = ~0u;
    uint32_t heads = geo[pick][0], secs = geo[pick][1], cyl = h->blocks / (heads * secs);
    r->cylinders = cyl; r->heads = heads; r->sectors = secs;
    r->nparts = 1;
    struct hdf_part *p = &r->part[0];
    p->flags = 0;
    uint32_t env[17] = { 16, 128, 0, heads, 1, secs, geo[pick][2], 0, 0, 0, cyl - 1, 30, 1, 0x1fe00, 0x7ffffffe, 0, dostype };
    for (int i = 0; i < 17; i++) p->envec[i] = env[i];
    ret = 1;
out:
    io->free(io->ctx, b, HDF_BLOCK);
    return ret;
}

/* ---- filesystems in the RDB ------------------------------------------------------ */
int hdf_rdb_fslist(struct hdf_io *io, const struct hdf_file *h, const struct hdf_rdb *r, struct hdf_fs *fs, int max)
{
    if (r->fshd == ~0u || !max) return 0;
    uint8_t *b = io->alloc(io->ctx, HDF_BLOCK);
    if (!b) return -1;
    int n = 0, guard = 32;
    uint32_t next = r->fshd;
    while (next != 0xffffffffu && guard-- && n < max) {
        if (hdf_read(io, h, next, 1, b) || be32(b) != 0x46534844u || !rdb_sum_ok(b)) {   /* 'FSHD' */
            hdf_log(io, "%s: broken filesystem list at block %lu", h->name, next);
            n = -1;
            break;
        }
        struct hdf_fs *f = &fs[n++];
        f->dostype = be32(b + 32);
        f->version = be32(b + 36);
        f->patchflags = be32(b + 40);
        for (int i = 0; i < 9; i++) f->dn[i] = be32(b + 44 + 4 * i);
        next = be32(b + 16);
    }
    io->free(io->ctx, b, HDF_BLOCK);
    return n;
}

int hdf_rdb_fsdata(struct hdf_io *io, const struct hdf_file *h, const struct hdf_fs *fs, uint8_t **data, uint32_t *size, uint32_t *alloc)
{
    uint8_t *b = io->alloc(io->ctx, HDF_BLOCK);
    if (!b) return -1;
    int ret = -1;
    uint32_t n = 0, next = fs->dn[7], guard = 8192;
    /* count the LSEG blocks, then read them into one buffer */
    while (next != 0xffffffffu && guard--) {
        if (hdf_read(io, h, next, 1, b) || be32(b) != 0x4c534547u || !rdb_sum_ok(b)) goto out;     /* 'LSEG' */
        n++;
        next = be32(b + 16);
    }
    if (!n || next != 0xffffffffu) goto out;
    *alloc = n * 492;
    *data = io->alloc(io->ctx, *alloc);
    if (!*data) goto out;
    *size = 0;
    next = fs->dn[7];
    for (uint32_t i = 0; i < n; i++) {
        if (hdf_read(io, h, next, 1, b)) { io->free(io->ctx, *data, *alloc); *data = 0; goto out; }
        uint32_t k = (be32(b + 4) - 5) * 4;                   /* SummedLongs covers the header's 5 longs and the data */
        if (k > 492) k = 492;
        copy(*data + *size, b + 20, k);
        *size += k;
        next = be32(b + 16);
    }
    ret = 0;
out:
    io->free(io->ctx, b, HDF_BLOCK);
    return ret;
}

/* ---- volume names ------------------------------------------------------------------ */
int hdf_volume(struct hdf_io *io, const struct hdf_file *h, const struct hdf_part *p, char name[32])
{
    const uint32_t *ev = p->envec;
    name[0] = 0;
    uint32_t dostype = ev[0] >= 16 ? ev[16] : 0x444f5300u;
    if ((dostype & 0xffffff00u) != 0x444f5300u || (dostype & 0xff) > 7) return 0;
    if (ev[0] < 10 || ev[1] != 128 || ev[4] != 1 || ev[9] > ev[10]) return 0;
    uint32_t bpc = ev[3] * ev[5], start = ev[9] * bpc, n = (ev[10] - ev[9] + 1) * bpc;
    uint32_t root = start + (ev[6] + n - 1) / 2;
    uint8_t *b = io->alloc(io->ctx, HDF_BLOCK);
    if (!b) return 0;
    int ret = 0;
    if (hdf_read(io, h, root, 1, b) == 0 && be32(b) == 2 && be32(b + 508) == 1 && b[432] && b[432] <= 30) {
        for (unsigned k = 0; k < b[432]; k++) name[k] = (char)b[433 + k];
        name[b[432]] = 0;
        ret = 1;
    }
    io->free(io->ctx, b, HDF_BLOCK);
    return ret;
}

int card_amiga_parts(struct hdf_io *io, struct hdf_file *out, int max)
{
    uint8_t *b = io->alloc(io->ctx, HDF_BLOCK);
    if (!b) return 0;
    int n = 0;
    if (io->read(io->ctx, 0, 1, b) == 0 && b[510] == 0x55 && b[511] == 0xaa) {
        for (int i = 0; i < 4 && n < max; i++) {
            const uint8_t *e = b + 446 + 16 * i;
            if (e[4] != 0x76 || !le32(e + 8) || le32(e + 12) < 64) continue;
            struct hdf_file *h = &out[n];
            zero(h, sizeof *h);
            h->ext = io->alloc(io->ctx, sizeof *h->ext);
            if (!h->ext) break;
            h->cap = h->next = 1;
            h->ext[0].file_blk = 0; h->ext[0].card_sec = le32(e + 8); h->ext[0].count = le32(e + 12);
            h->blocks = le32(e + 12);
            h->size = h->blocks < 0x800000u ? h->blocks * HDF_BLOCK : 0xffffffffu;
            const char *t = "SD card partition 1";
            unsigned k = 0;
            for (; t[k]; k++) h->name[k] = t[k];
            h->name[k - 1] = (char)('1' + i);
            h->name[k] = 0;
            n++;
        }
    }
    io->free(io->ctx, b, HDF_BLOCK);
    return n;
}
