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
    if (f->fat32) f->root_clus = le32(s + 44);
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
struct scan { struct hdf_file *files; int max, n; uint32_t hdf_dir; };

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
            if (!(e[11] & 0x08)) {                            /* not a volume label */
                if (e[11] & 0x10) {
                    if (top && !sc->hdf_dir && e[0] == 'H' && e[1] == 'D' && e[2] == 'F' && e[3] == ' ' && e[8] == ' ')
                        sc->hdf_dir = le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0);
                } else if ((e[8] == 'H' || e[8] == 'h') && (e[9] == 'D' || e[9] == 'd') && (e[10] == 'F' || e[10] == 'f')) {
                    take(f, sc, e, ln);
                }
            }
            lname[0] = 0;
        }
    }
}

int fat_find_hdfs(struct fat *f, struct hdf_file *files, int max)
{
    struct scan sc = { files, max, 0, 0 };
    scan_dir(f, &sc, f->fat32 ? f->root_clus : 0, 1);
    if (sc.hdf_dir) scan_dir(f, &sc, sc.hdf_dir, 0);
    return sc.n;
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
    uint32_t at = ~0u;
    for (uint32_t i = 0; i < 16 && i < h->blocks; i++) {      /* RDB_LOCATION_LIMIT */
        if (hdf_read(io, h, i, 1, b)) { ret = -1; goto out; }
        if (be32(b) == 0x5244534bu && rdb_sum_ok(b)) { at = i; break; }   /* 'RDSK' */
    }
    if (at == ~0u) goto out;
    if (be32(b + 16) != HDF_BLOCK) { hdf_log(io, "%s: RDB block size %lu: only 512 is supported", h->name, be32(b + 16)); ret = -1; goto out; }
    r->cylinders = be32(b + 64); r->sectors = be32(b + 68); r->heads = be32(b + 72);
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
