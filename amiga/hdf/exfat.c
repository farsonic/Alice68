/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: reading the SD card's exFAT partition (exfat.h). From Microsoft's
 * exFAT specification: the Main Boot Sector and its checksum (sections 3.1,
 * 3.4), the FAT (4), the cluster heap (5), directory entry sets (6, 7: File,
 * Stream Extension, File Name). Clusters are allocated in the Allocation
 * Bitmap; the FAT only chains files that are not contiguous (NoFatChain).
 */
#include "exfat.h"

#define T_END      0x00
#define T_BITMAP   0x81
#define T_LABEL    0x83
#define T_FILE     0x85
#define T_STREAM   0xc0
#define T_NAME     0xc1

static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static int is_exfat(const uint8_t *s)
{
    static const char oem[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    for (int i = 0; i < 8; i++) if (s[3 + i] != (uint8_t)oem[i]) return 0;
    return 1;
}

static uint32_t clus_sec(const struct xfat *v, uint32_t c) { return v->heap_start + ((c - 2) << v->spc_shift); }
static int clus_ok(const struct xfat *v, uint32_t c) { return c >= 2 && c - 2 < v->clusters; }

/* the next cluster in a FAT chain; 0 = end, bad, unreadable */
static uint32_t fat_next(struct xfat *v, uint32_t c)
{
    uint32_t sec = v->fat_start + c / 128;
    if (v->fwin_first == ~0u || sec < v->fwin_first || sec >= v->fwin_first + XFAT_FATWIN) {
        if (v->io->read(v->io->ctx, sec, XFAT_FATWIN, v->fwin)) { v->fwin_first = ~0u; return 0; }
        v->fwin_first = sec;
    }
    uint32_t n = le32(v->fwin + (sec - v->fwin_first) * 512 + (c % 128) * 4);
    return clus_ok(v, n) ? n : 0;
}

static int read_sec(struct xfat *v, uint32_t sec)
{
    if (sec == v->sec_no) return 0;
    if (v->io->read(v->io->ctx, sec, 1, v->sec)) { v->sec_no = ~0u; return -2; }
    v->sec_no = sec;
    return 0;
}

/* the ci-th cluster of a directory; 0 = past its end */
static uint32_t dir_clus(struct xfat *v, const struct xfat_dir *d, uint32_t ci)
{
    uint32_t cbytes_shift = 9 + v->spc_shift;
    if (d->size && (uint64_t)ci << cbytes_shift >= d->size) return 0;
    if (d->nofat) return clus_ok(v, d->first + ci) ? d->first + ci : 0;
    if (v->wc_first != d->first || v->wc_ci > ci) { v->wc_first = d->first; v->wc_ci = 0; v->wc_c = d->first; }
    while (v->wc_ci < ci && v->wc_c) { v->wc_c = fat_next(v, v->wc_c); v->wc_ci++; }
    return clus_ok(v, v->wc_c) ? v->wc_c : 0;
}

/* entry idx of a directory, in v->sec: 0 ok (*p set), -1 past the end, -2 card error */
static int entry(struct xfat *v, const struct xfat_dir *d, uint32_t idx, const uint8_t **p)
{
    uint64_t off = (uint64_t)idx * 32;
    uint32_t cshift = 9 + v->spc_shift;
    if (d->size && off >= d->size) return -1;
    uint32_t c = dir_clus(v, d, (uint32_t)(off >> cshift));
    if (!c) return -1;
    uint32_t within = (uint32_t)(off & ((1u << cshift) - 1));
    int r = read_sec(v, clus_sec(v, c) + within / 512);
    if (r) return r;
    *p = v->sec + within % 512;
    return 0;
}

int xfat_open(struct xfat *v, struct hdf_io *io)
{
    zero(v, sizeof *v);
    v->io = io;
    v->sec_no = v->fwin_first = ~0u;
    v->sec = io->alloc(io->ctx, 512);
    v->fwin = io->alloc(io->ctx, XFAT_FATWIN * 512);
    if (!v->sec || !v->fwin) { xfat_close(v); return -1; }
    uint8_t *s = v->sec;
    if (io->read(io->ctx, 0, 1, s) || s[510] != 0x55 || s[511] != 0xaa) { xfat_close(v); return -1; }
    uint32_t start = ~0u;
    if (is_exfat(s)) start = 0;                                  /* no partition table: exFAT from sector 0 */
    else {
        uint32_t starts[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s + 446 + 16 * i;
            if (e[4] && e[4] != 0xee && e[4] != 0x76 && e[4] != 0x05 && e[4] != 0x0f && le32(e + 8)) starts[i] = le32(e + 8);
        }
        for (int i = 0; i < 4 && start == ~0u; i++)
            if (starts[i] && !io->read(io->ctx, starts[i], 1, s) && is_exfat(s)) start = starts[i];
    }
    v->sec_no = ~0u;
    if (start == ~0u) { xfat_close(v); return -1; }               /* none: not an error */
    if (io->read(io->ctx, start, 1, s)) { hdf_log(io, "exFAT partition at %lu: unreadable", start); xfat_close(v); return -1; }

    /* the Main Boot Sector */
    int ok = s[0] == 0xeb && s[1] == 0x76 && s[2] == 0x90 && s[510] == 0x55 && s[511] == 0xaa;
    for (int i = 11; i < 64 && ok; i++) if (s[i]) ok = 0;
    if (!ok) { hdf_log(io, "exFAT partition at %lu: not a valid boot sector", start); xfat_close(v); return -1; }
    if (s[108] != 9) { hdf_log(io, "exFAT partition at %lu: %lu-byte sectors, not supported", start, (uint32_t)1 << s[108]); xfat_close(v); return -1; }
    uint32_t fat_off = le32(s + 80), fat_len = le32(s + 84), heap = le32(s + 88), count = le32(s + 92), root = le32(s + 96);
    uint32_t nfats = s[110], shift = s[109];
    uint64_t vol_len = le64(s + 72);
    v->flags = (uint16_t)le16(s + 106);
    if (shift > 16 || (nfats != 1 && nfats != 2) || fat_off < 24 || !fat_len || heap < fat_off + nfats * fat_len ||
        !count || (uint64_t)fat_len * 128 < (uint64_t)count + 2 || (uint64_t)heap + ((uint64_t)count << shift) > vol_len ||
        root < 2 || root - 2 >= count) {
        hdf_log(io, "exFAT partition at %lu: bad geometry", start); xfat_close(v); return -1;
    }
    /* the boot checksum: sectors 0..10 of the boot region, VolumeFlags and PercentInUse left out */
    uint32_t sum = 0;
    for (uint32_t k = 0; k < 11; k++) {
        if (k && io->read(io->ctx, start + k, 1, s)) { hdf_log(io, "exFAT partition at %lu: boot region unreadable", start); xfat_close(v); return -1; }
        for (int i = 0; i < 512; i++) {
            if (k == 0 && (i == 106 || i == 107 || i == 112)) continue;
            sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + s[i];
        }
    }
    if (io->read(io->ctx, start + 11, 1, s) || le32(s) != sum || le32(s + 508) != sum) {
        hdf_log(io, "exFAT partition at %lu: boot checksum wrong - ignored (run a disk check on the card)", start); xfat_close(v); return -1;
    }
    v->part_start = start;
    v->fat_start = start + fat_off + ((v->flags & 1) && nfats == 2 ? fat_len : 0);
    v->heap_start = start + heap;
    v->spc_shift = shift;
    v->clusters = count;
    v->root = root;

    /* the volume label and the Allocation Bitmap, from the root directory */
    v->nfree = ~0u;
    struct xfat_dir d;
    xfat_root(v, &d);
    for (uint32_t i = 0; i < 4096; i++) {
        const uint8_t *p;
        if (entry(v, &d, i, &p) || p[0] == T_END) break;
        if (p[0] == T_BITMAP && !(p[1] & 1) && !v->bm_first) { v->bm_first = le32(p + 20); v->bm_len = le64(p + 24); }
        if (p[0] == T_LABEL && !v->label[0]) {
            uint32_t n = p[1] > 11 ? 11 : p[1];
            for (uint32_t k = 0; k < n; k++) { uint32_t ch = le16(p + 2 + 2 * k); v->label[k] = (char)(ch < 0x100 ? ch : '?'); }
            v->label[n] = 0;
        }
    }
    hdf_log(io, "exFAT partition at sector %lu, %lu clusters of %lu bytes%s%s%s", start, count, (uint32_t)512 << shift,
            v->label[0] ? ", \"" : "", v->label, v->label[0] ? "\"" : "");
    if (v->flags & 2) hdf_log(io, "exFAT: the volume was not cleanly removed (run a disk check on the card)");
    return 0;
}

void xfat_close(struct xfat *v)
{
    if (v->sec) v->io->free(v->io->ctx, v->sec, 512);
    if (v->fwin) v->io->free(v->io->ctx, v->fwin, XFAT_FATWIN * 512);
    if (v->bounce) v->io->free(v->io->ctx, v->bounce, 512);
    if (v->dirs) v->io->free(v->io->ctx, v->dirs, v->capdirs * sizeof *v->dirs);
    v->sec = v->fwin = v->bounce = 0;
    v->dirs = 0; v->ndirs = v->capdirs = 0;
}

void xfat_root(const struct xfat *v, struct xfat_dir *d) { d->first = v->root; d->size = 0; d->nofat = 0; }
void xfat_ent_dir(const struct xfat_ent *e, struct xfat_dir *d) { d->first = e->first; d->size = e->size; d->nofat = e->nofat; }

int xfat_next(struct xfat *v, const struct xfat_dir *d, uint32_t *pos, struct xfat_ent *e)
{
    for (uint32_t guard = 0; guard < 0x100000; guard++) {
        const uint8_t *p;
        int r = entry(v, d, *pos, &p);
        if (r) return r == -1 ? -1 : -2;
        if (p[0] == T_END) return -1;
        if (p[0] != T_FILE) { (*pos)++; continue; }          /* bitmap, up-case table, label, deleted, ... */
        /* the set entry by entry (no copy of it: the device starts on a small stack),
           its checksum as it goes; entry k may be in another sector than entry k-1 */
        uint32_t n = 1u + p[1], at = *pos, namelen = 0, got = 0, ok = 1;
        if (n < 3 || n > 19) { (*pos)++; continue; }
        uint16_t sum = 0, want = (uint16_t)le16(p + 2);
        zero(e, sizeof *e);
        e->pos = at;
        e->count = n;
        e->attr = (uint16_t)le16(p + 4);
        e->mtime = le32(p + 12);
        for (uint32_t k = 0; k < n; k++) {
            if (k && (r = entry(v, d, at + k, &p))) return r == -1 ? -3 : -2;
            for (int i = 0; i < 32; i++) {
                if (k == 0 && (i == 2 || i == 3)) continue;
                sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + p[i]);
            }
            if (k == 1) {
                if (p[0] != T_STREAM) ok = 0;
                namelen = p[3];
                e->nofat = (p[1] & 2) != 0;
                e->valid = le64(p + 8);
                e->first = le32(p + 20);
                e->size = le64(p + 24);
                if (!(p[1] & 1)) { e->first = 0; e->size = e->valid = 0; }   /* no clusters */
            } else if (k >= 2 && p[0] == T_NAME) {
                for (int i = 0; i < 15 && got < namelen; i++, got++) {
                    uint32_t ch = le16(p + 2 + 2 * i);
                    if (got < XFAT_NAME - 1) e->name[got] = (char)(ch < 0x100 ? ch : '?');
                }
            }
        }
        *pos = at + n;
        if (!ok || sum != want) continue;                    /* a broken set: skipped */
        if (got >= XFAT_NAME) e->name[XFAT_NAME - 1] = 0;
        if (!got) continue;
        return 0;
    }
    return -3;
}

static uint8_t fold(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) return (uint8_t)(c - 32);
    return c;
}

int xfat_find(struct xfat *v, const struct xfat_dir *d, const char *name, uint32_t len, struct xfat_ent *e)
{
    uint32_t pos = 0;
    while (xfat_next(v, d, &pos, e) == 0) {
        uint32_t k = 0;
        while (k < len && e->name[k] && fold((uint8_t)e->name[k]) == fold((uint8_t)name[k])) k++;
        if (k == len && !e->name[k]) return 0;
    }
    return -1;
}

static int add_extent(struct hdf_io *io, struct hdf_file *h, uint32_t file_blk, uint32_t sec, uint32_t n)
{
    if (h->next && h->ext[h->next - 1].card_sec + h->ext[h->next - 1].count == sec) {
        h->ext[h->next - 1].count += n;
        return 0;
    }
    if (h->next == h->cap) {
        uint32_t cap = h->cap ? h->cap * 2 : 16;
        struct hdf_extent *x = io->alloc(io->ctx, cap * sizeof *x);
        if (!x) return -1;
        if (h->ext) { copy(x, h->ext, h->next * sizeof *x); io->free(io->ctx, h->ext, h->cap * sizeof *x); }
        h->ext = x;
        h->cap = cap;
    }
    h->ext[h->next].file_blk = file_blk;
    h->ext[h->next].card_sec = sec;
    h->ext[h->next].count = n;
    h->next++;
    return 0;
}

int xfat_extents(struct xfat *v, const struct xfat_ent *e, uint32_t blocks, struct hdf_file *h)
{
    uint32_t spc = 1u << v->spc_shift, have = 0, c = e->first, guard = v->clusters + 1;
    if ((uint64_t)blocks * 512 > e->size) return -1;
    while (have < blocks) {
        if (!clus_ok(v, c) || !guard--) return -1;
        uint32_t n = blocks - have < spc ? blocks - have : spc;
        if (add_extent(v->io, h, have, clus_sec(v, c), n)) return -1;
        have += n;
        if (have < blocks) c = e->nofat ? c + 1 : fat_next(v, c);
    }
    return 0;
}

/* name ends in .hdf / .iso, any case */
static int has_ext(const char *n, const char *x)
{
    uint32_t l = 0;
    while (n[l]) l++;
    if (l < 5) return 0;
    for (int i = 0; i < 4; i++) if (fold((uint8_t)n[l - 4 + i]) != fold((uint8_t)x[i])) return 0;
    return 1;
}

static void take(struct xfat *v, int iso, const struct xfat_ent *e, struct hdf_file *files, int max, int *n, int *told)
{
    if (*n >= max) {
        if (!*told) hdf_log(v->io, "more than %d %s: the rest are ignored", max, iso ? "CD images" : "hardfiles");
        *told = 1;
        return;
    }
    struct hdf_file *h = &files[*n];
    zero(h, sizeof *h);
    uint32_t k = 0;
    for (; e->name[k] && k < HDF_NAME - 1; k++) h->name[k] = e->name[k];
    h->name[k] = 0;
    if (e->valid < e->size) {
        hdf_log(v->io, "%s: not completely written on the card (valid length below its size) - ignored", h->name);
        return;
    }
    uint64_t blocks = e->size / HDF_BLOCK;
    if (blocks > 0xffffffffu) { hdf_log(v->io, "%s: over 2 TB - ignored", h->name); return; }
    h->blocks = (uint32_t)blocks;
    h->size = e->size > 0xffffffffu ? 0xffffffffu : (uint32_t)e->size;
    if (e->size % HDF_BLOCK) hdf_log(v->io, "%s: size is not a multiple of 512 - the tail is ignored", h->name);
    if (h->blocks < 2) { hdf_log(v->io, "%s: too small, ignored", h->name); return; }
    if (xfat_extents(v, e, h->blocks, h)) {
        hdf_log(v->io, "%s: its clusters do not cover the file - ignored (run a disk check on the card)", h->name);
        hdf_file_free(v->io, h);
        return;
    }
    hdf_log(v->io, "%s: %lu MB, %lu piece%s on the card (exFAT)", h->name, h->blocks >> 11, h->next, h->next == 1 ? "" : "s");
    (*n)++;
}

int xfat_find_files(struct xfat *v, int iso, struct hdf_file *files, int max)
{
    const char *ext = iso ? ".iso" : ".hdf", *dirname = iso ? "ISO" : "HDF";
    struct xfat_dir root, sub;
    struct xfat_ent e;
    int n = 0, told = 0, have_sub = 0;
    xfat_root(v, &root);
    uint32_t pos = 0;
    while (xfat_next(v, &root, &pos, &e) == 0) {
        if (e.attr & XFAT_A_DIR) {
            if (!have_sub && fold((uint8_t)e.name[0]) == (uint8_t)dirname[0] && fold((uint8_t)e.name[1]) == (uint8_t)dirname[1] &&
                fold((uint8_t)e.name[2]) == (uint8_t)dirname[2] && !e.name[3] && e.first) { xfat_ent_dir(&e, &sub); have_sub = 1; }
        } else if (e.name[0] != '.' && has_ext(e.name, ext)) take(v, iso, &e, files, max, &n, &told);
    }
    if (have_sub) {
        pos = 0;
        while (xfat_next(v, &sub, &pos, &e) == 0)
            if (!(e.attr & XFAT_A_DIR) && e.name[0] != '.' && has_ext(e.name, ext)) take(v, iso, &e, files, max, &n, &told);
    }
    return n;
}

/* ---- folders: the ffs_* calls on exFAT (fatfs.h's types; read-only so far) ---------------------- */

static struct xdir *xd_get(struct xfat *x, uint32_t first)
{
    for (uint32_t i = x->ndirs; i-- > 0; ) if (x->dirs[i].first == first) return &x->dirs[i];
    return 0;
}
/* a directory seen in its parent: remember where it is */
static int xd_note(struct xfat *x, uint32_t first, uint32_t parent, uint32_t pos, int nofat, uint64_t size)
{
    struct xdir *d = xd_get(x, first);
    if (!d) {
        if (x->ndirs == x->capdirs) {
            uint32_t cap = x->capdirs ? x->capdirs * 2 : 64;
            struct xdir *n = x->io->alloc(x->io->ctx, cap * sizeof *n);
            if (!n) return FFS_IO;
            if (x->dirs) { copy(n, x->dirs, x->ndirs * sizeof *n); x->io->free(x->io->ctx, x->dirs, x->capdirs * sizeof *n); }
            x->dirs = n; x->capdirs = cap;
        }
        d = &x->dirs[x->ndirs++];
        d->first = first;
    }
    d->parent = parent; d->pos = pos; d->nofat = (uint32_t)nofat; d->size = size;
    return 0;
}
static int xd_dir(struct xfat *x, uint32_t key, struct xfat_dir *d)
{
    if (key == x->root) { xfat_root(x, d); return 0; }
    struct xdir *k = xd_get(x, key);
    if (!k) return FFS_CORRUPT;                          /* never seen in its parent */
    d->first = k->first; d->size = k->size; d->nofat = (int)k->nofat;
    return 0;
}
static int xerr(int r) { return r == -1 ? FFS_END : r == -2 ? FFS_IO : FFS_CORRUPT; }

int ffs_init_exfat(struct ffs *v, struct xfat *x)
{
    zero(v, sizeof *v);
    v->x = x;
    v->date = (uint16_t)((46 << 9) | (1 << 5) | 1);
    if (!x->bounce && !(x->bounce = x->io->alloc(x->io->ctx, 512))) return FFS_IO;
    return xd_note(x, x->root, x->root, 0, 0, 0);
}
void xfs_done(struct ffs *v) { (void)v; }
int xfs_sync(struct ffs *v) { int e = v->err; v->err = 0; return e; }
void xfs_set_io(struct ffs *v, struct hdf_io *io) { v->x->io = io; }
uint32_t xfs_cluster_bytes(const struct ffs *v) { return (uint32_t)512 << v->x->spc_shift; }
uint32_t xfs_clusters(const struct ffs *v) { return v->x->clusters; }
uint32_t xfs_clus_sec(const struct ffs *v, uint32_t c) { return clus_sec(v->x, c); }

int xfs_next(struct ffs *v, uint32_t dir, uint32_t *pos, struct ffs_ent *o)
{
    struct xfat *x = v->x;
    struct xfat_dir d;
    struct xfat_ent e;
    int r = xd_dir(x, dir, &d);
    if (r) return r;
    if ((r = xfat_next(x, &d, pos, &e))) return xerr(r);
    const uint8_t *p;
    if ((r = entry(x, &d, e.pos, &p))) return xerr(r);   /* where its File entry is: the entry's identity */
    zero(o, sizeof *o);
    o->dir = dir;
    o->idx = e.pos;
    o->nlfn = e.count;
    o->sec = x->sec_no;
    o->slot = e.pos % 16;
    o->first = e.first;
    o->attr = (uint8_t)e.attr;
    o->date = (uint16_t)(e.mtime >> 16);
    o->time = (uint16_t)e.mtime;
    o->xnofat = (uint8_t)e.nofat;
    o->xsize = e.size;
    o->xvalid = e.valid;
    o->size = e.size > 0xffffffffu ? 0xffffffffu : (uint32_t)e.size;
    copy(o->name, e.name, FFS_NAME);
    if (e.attr & XFAT_A_DIR) {
        if (!e.first) return FFS_CORRUPT;                /* a directory always has a cluster */
        if ((r = xd_note(x, e.first, dir, e.pos, e.nofat, e.size))) return r;
    }
    return 0;
}

int xfs_find(struct ffs *v, uint32_t dir, const char *name, uint32_t len, struct ffs_ent *e)
{
    uint32_t pos = 0;
    for (;;) {
        int r = xfs_next(v, dir, &pos, e);
        if (r) return r;
        if (ffs_name_eq(e->name, name, len)) return 0;
    }
}

int xfs_parent(struct ffs *v, uint32_t dir, uint32_t *parent)
{
    struct xfat *x = v->x;
    if (dir == x->root) { *parent = x->root; return FFS_END; }
    struct xdir *k = xd_get(x, dir);
    if (!k) return FFS_CORRUPT;
    *parent = k->parent;
    return 0;
}

int xfs_dir_ent(struct ffs *v, uint32_t dir, struct ffs_ent *e)
{
    struct xfat *x = v->x;
    struct xdir *k = xd_get(x, dir);
    if (!k || dir == x->root) return FFS_CORRUPT;
    uint32_t pos = k->pos;
    int r = xfs_next(v, k->parent, &pos, e);
    if (r) return r == FFS_END ? FFS_CORRUPT : r;
    return (e->attr & FFS_A_DIR) && e->first == dir ? 0 : FFS_CORRUPT;
}

int xfs_resolve(struct ffs *v, const char *path, uint32_t *dir, struct ffs_ent *e)
{
    uint32_t d = v->x->root;
    int any = 0;
    while (*path) {
        while (*path == '/') path++;
        uint32_t n = 0;
        while (path[n] && path[n] != '/') n++;
        if (!n) break;
        int r = xfs_find(v, d, path, n, e);
        if (r) return r;
        if (!(e->attr & FFS_A_DIR)) return FFS_NOTDIR;
        d = e->first;
        path += n;
        any = 1;
    }
    if (!any) return FFS_BADNAME;
    *dir = d;
    return 0;
}

/* writing: stage 3 */
int xfs_create(struct ffs *v, uint32_t dir, const char *name, uint32_t len, int isdir, struct ffs_ent *o)
{ (void)v; (void)dir; (void)name; (void)len; (void)isdir; (void)o; return FFS_WP; }
int xfs_delete(struct ffs *v, const struct ffs_ent *e) { (void)v; (void)e; return FFS_WP; }
int xfs_rename(struct ffs *v, const struct ffs_ent *e, uint32_t ndir, const char *name, uint32_t len, struct ffs_ent *o)
{ (void)v; (void)e; (void)ndir; (void)name; (void)len; (void)o; return FFS_WP; }
int xfs_set_attr(struct ffs *v, struct ffs_ent *e, uint8_t attr) { (void)v; (void)e; (void)attr; return FFS_WP; }
int xfs_set_date(struct ffs *v, struct ffs_ent *e, uint16_t date, uint16_t time) { (void)v; (void)e; (void)date; (void)time; return FFS_WP; }
int xfs_flush_file(struct ffs *v, struct ffs_file *h) { (void)v; return h->dirty ? FFS_WP : 0; }
int32_t xfs_write(struct ffs *v, struct ffs_file *h, uint32_t pos, const void *buf, uint32_t n, int *err)
{ (void)v; (void)h; (void)pos; (void)buf; (void)n; *err = FFS_WP; return 0; }
int xfs_truncate(struct ffs *v, struct ffs_file *h, uint32_t size) { (void)v; (void)h; (void)size; return FFS_WP; }

/* the file's ci-th cluster, 0 = past its clusters */
static uint32_t file_clus(struct xfat *x, struct ffs_file *h, uint32_t ci)
{
    if (!h->e.first) return 0;
    if (h->e.xnofat) return ci < x->clusters && clus_ok(x, h->e.first + ci) ? h->e.first + ci : 0;
    uint32_t k = 0, c = h->e.first;
    if (h->ci != ~0u && h->ci <= ci) { k = h->ci; c = h->cc; }
    while (k < ci && c) { c = fat_next(x, c); k++; }
    if (!c) return 0;
    h->ci = k; h->cc = c;
    return c;
}

int32_t xfs_read(struct ffs *v, struct ffs_file *h, uint32_t pos, void *buf, uint32_t n)
{
    struct xfat *x = v->x;
    uint8_t *p = buf;
    uint64_t size = h->e.xsize > 0xffffffffu ? 0xffffffffu : h->e.xsize;
    if (pos >= size) return 0;
    if (n > size - pos) n = (uint32_t)(size - pos);
    uint32_t cshift = 9 + x->spc_shift, done = 0;
    while (done < n) {
        uint32_t left = n - done, off = pos & 511;
        if (pos >= h->e.xvalid) {                        /* past ValidDataLength: zeros */
            for (uint32_t k = 0; k < left; k++) p[k] = 0;
            return (int32_t)n;
        }
        uint32_t c = file_clus(x, h, pos >> cshift);
        if (!c) return done ? (int32_t)done : FFS_CORRUPT;
        uint32_t in = pos & ((1u << cshift) - 1), sec = clus_sec(x, c) + in / 512;
        uint32_t avail = (1u << cshift) - in;            /* bytes to the end of this cluster */
        if (avail > left) avail = left;
        if ((uint64_t)pos + avail > h->e.xvalid) avail = (uint32_t)(h->e.xvalid - pos);
        if (!off && avail >= 512 && !((uintptr_t)p & 3)) {
            uint32_t cnt = avail / 512;
            if (x->io->read(x->io->ctx, sec, cnt, p)) { v->err = FFS_IO; return done ? (int32_t)done : FFS_IO; }
            avail = cnt * 512;
        } else {
            if (x->io->read(x->io->ctx, sec, 1, x->bounce)) { v->err = FFS_IO; return done ? (int32_t)done : FFS_IO; }
            if (avail > 512 - off) avail = 512 - off;
            copy(p, x->bounce + off, avail);
        }
        p += avail; pos += avail; done += avail;
    }
    return (int32_t)done;
}

int xfs_free_clusters(struct ffs *v, uint32_t *n)
{
    struct xfat *x = v->x;
    if (x->nfree == ~0u) {
        if (!x->bm_first) return FFS_CORRUPT;
        uint32_t used = 0, bits = x->clusters, c = x->bm_first, done = 0, spc = 1u << x->spc_shift;
        while (done < bits && c) {
            for (uint32_t k = 0; k < spc && done < bits; k++) {
                if (x->io->read(x->io->ctx, clus_sec(x, c) + k, 1, x->bounce)) return FFS_IO;
                for (uint32_t b = 0; b < 512 && done < bits; b++) {
                    uint8_t m = x->bounce[b];
                    for (int i = 0; i < 8 && done < bits; i++, done++) used += (m >> i) & 1;
                }
            }
            c = fat_next(x, c);
        }
        x->nfree = bits - used;
    }
    *n = x->nfree;
    return 0;
}
