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
#define T_UPCASE   0x82
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

static int fwin_flush(struct xfat *v);
/* the FAT entry of cluster c in the window (loaded, never past the FAT's end); 0 = card error */
static uint8_t *fat_ptr(struct xfat *v, uint32_t c)
{
    uint32_t sec = v->fat_start + c / 128;
    if (sec >= v->fat_end) return 0;
    if (v->fwin_first == ~0u || sec < v->fwin_first || sec >= v->fwin_first + v->fwin_n) {
        if (fwin_flush(v)) return 0;
        uint32_t n = v->fat_end - sec < XFAT_FATWIN ? v->fat_end - sec : XFAT_FATWIN;
        if (v->io->read(v->io->ctx, sec, n, v->fwin)) { v->fwin_first = ~0u; return 0; }
        v->fwin_first = sec; v->fwin_n = n;
    }
    return v->fwin + (sec - v->fwin_first) * 512 + (c % 128) * 4;
}
/* the next cluster in a FAT chain; 0 = end, bad, unreadable */
static uint32_t fat_next(struct xfat *v, uint32_t c)
{
    uint8_t *p = fat_ptr(v, c);
    if (!p) return 0;
    uint32_t n = le32(p);
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
    v->bounce = io->alloc(io->ctx, 512);
    if (!v->sec || !v->fwin || !v->bounce) { xfat_close(v); return -1; }
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
    v->nfats = nfats;
    v->bm_lo = ~0u;
    v->fat_end = v->fat_start + fat_len;
    if (v->flags & 4) v->ro = "the volume reports a media failure";
    else if (nfats != 1) v->ro = "two FATs (TexFAT): not written here";

    /* the volume label and the Allocation Bitmap, from the root directory */
    v->nfree = ~0u;
    struct xfat_dir d;
    xfat_root(v, &d);
    for (uint32_t i = 0; i < 4096; i++) {
        const uint8_t *p;
        if (entry(v, &d, i, &p) || p[0] == T_END) break;
        if (p[0] == T_BITMAP && !(p[1] & 1) && !v->bm_first) { v->bm_first = le32(p + 20); v->bm_len = le64(p + 24); }
        if (p[0] == T_UPCASE && !v->up_first) { v->up_first = le32(p + 20); v->up_len = le64(p + 24); }
        if (p[0] == T_LABEL && !v->label[0]) {
            uint32_t n = p[1] > 11 ? 11 : p[1];
            for (uint32_t k = 0; k < n; k++) { uint32_t ch = le16(p + 2 + 2 * k); v->label[k] = (char)(ch < 0x100 ? ch : '?'); }
            v->label[n] = 0;
        }
    }
    hdf_log(io, "exFAT partition at sector %lu, %lu clusters of %lu bytes%s%s%s", start, count, (uint32_t)512 << shift,
            v->label[0] ? ", \"" : "", v->label, v->label[0] ? "\"" : "");
    if (v->flags & 2) hdf_log(io, "exFAT: the volume was not cleanly removed");
    return 0;
}

void xfat_close(struct xfat *v)
{
    if (v->sec) v->io->free(v->io->ctx, v->sec, 512);
    if (v->fwin) v->io->free(v->io->ctx, v->fwin, XFAT_FATWIN * 512);
    if (v->bounce) v->io->free(v->io->ctx, v->bounce, 512);
    if (v->dirs) v->io->free(v->io->ctx, v->dirs, v->capdirs * sizeof *v->dirs);
    if (v->bm) v->io->free(v->io->ctx, v->bm, (uint32_t)v->bm_len);
    v->bm = 0;
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

/* ---- writing ---------------------------------------------------------------------------------- */
#define EOC 0xffffffffu

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t cbytes(const struct xfat *x) { return (uint32_t)512 << x->spc_shift; }
static uint32_t nclus(const struct xfat *x, uint64_t bytes) { return (uint32_t)((bytes + cbytes(x) - 1) >> (9 + x->spc_shift)); }

static int werr(struct xfat *x, int r)
{
    int e = r == -2 ? FFS_WP : FFS_IO;
    if (!x->err) x->err = e;
    return e;
}
static int wsec(struct xfat *x, uint32_t sec, uint32_t n, const void *buf)
{
    if (!x->io->write) return werr(x, -2);
    if (buf != x->sec && x->sec_no != ~0u && x->sec_no - sec < n) x->sec_no = ~0u;   /* the cached directory sector is older */
    int r = x->io->write(x->io->ctx, sec, n, buf);
    return r ? werr(x, r) : 0;
}

/* VolumeDirty on the card while changes are on their way (outside the boot checksum) */
static int set_dirty(struct xfat *x, int on)
{
    if (x->dirty_on_card == on) return 0;
    if (x->io->read(x->io->ctx, x->part_start, 1, x->bounce)) return werr(x, -1);
    uint32_t f = le16(x->bounce + 106);
    put16(x->bounce + 106, on ? f | 2 : f & ~2u);
    int r = wsec(x, x->part_start, 1, x->bounce);
    if (!r) x->dirty_on_card = on;
    return r;
}

static int fwin_flush(struct xfat *v)
{
    if (!v->fwin_dirty) return 0;
    int r = set_dirty(v, 1);
    if (!r) r = wsec(v, v->fwin_first, v->fwin_n, v->fwin);
    if (!r) v->fwin_dirty = 0;
    return r;
}
static int fat_set(struct xfat *x, uint32_t c, uint32_t val)
{
    uint8_t *p = fat_ptr(x, c);
    if (!p) return x->err ? x->err : FFS_IO;
    put32(p, val);
    x->fwin_dirty = 1;
    x->wc_first = 0;                                         /* chains changed: walk again */
    return 0;
}

/* the whole Allocation Bitmap in memory (it follows its FAT chain) */
static int bm_load(struct xfat *x)
{
    if (x->bm) return 0;
    if (!x->bm_first || x->bm_len < (x->clusters + 7) / 8 || x->bm_len > 0x10000000u) return FFS_CORRUPT;
    uint32_t len = (uint32_t)x->bm_len, done = 0, c = x->bm_first, spc = 1u << x->spc_shift;
    uint8_t *b = x->io->alloc(x->io->ctx, len);
    if (!b) return FFS_IO;
    while (done < len && clus_ok(x, c)) {
        for (uint32_t k = 0; k < spc && done < len; k++) {
            if (x->io->read(x->io->ctx, clus_sec(x, c) + k, 1, x->bounce)) { x->io->free(x->io->ctx, b, len); return FFS_IO; }
            uint32_t n = len - done < 512 ? len - done : 512;
            copy(b + done, x->bounce, n);
            done += n;
        }
        c = fat_next(x, c);
    }
    if (done < len) { x->io->free(x->io->ctx, b, len); return FFS_CORRUPT; }
    x->bm = b;
    uint32_t used = 0;
    for (uint32_t i = 0; i < x->clusters; i++) used += (b[i >> 3] >> (i & 7)) & 1;
    x->nfree = x->clusters - used;
    return 0;
}
static int bm_get(const struct xfat *x, uint32_t c) { uint32_t i = c - 2; return (x->bm[i >> 3] >> (i & 7)) & 1; }
static void bm_set(struct xfat *x, uint32_t c, int on)
{
    uint32_t i = c - 2;
    uint8_t m = (uint8_t)(1u << (i & 7));
    if (!!(x->bm[i >> 3] & m) == !!on) return;
    if (on) { x->bm[i >> 3] |= m; x->nfree--; } else { x->bm[i >> 3] &= (uint8_t)~m; x->nfree++; }
    uint32_t b = i >> 3;
    if (x->bm_lo == ~0u || b < x->bm_lo) x->bm_lo = b;
    if (x->bm_lo == ~0u || b + 1 > x->bm_hi) x->bm_hi = b + 1;
}
static int bm_flush(struct xfat *x)
{
    if (x->bm_lo == ~0u) return 0;
    int r = set_dirty(x, 1);
    if (r) return r;
    uint32_t cb = cbytes(x), len = (uint32_t)x->bm_len;
    for (uint32_t off = x->bm_lo & ~511u; off < x->bm_hi; off += 512) {
        uint32_t ci = off / cb, c = x->bm_first;                  /* the bitmap's ci-th cluster */
        for (uint32_t k = 0; k < ci && c; k++) c = fat_next(x, c);
        if (!clus_ok(x, c)) return werr(x, -1);
        uint32_t sec = clus_sec(x, c) + (off % cb) / 512;
        if (off + 512 <= len && !((uintptr_t)(x->bm + off) & 3)) r = wsec(x, sec, 1, x->bm + off);
        else {
            zero(x->bounce, 512);
            copy(x->bounce, x->bm + off, len - off < 512 ? len - off : 512);
            r = wsec(x, sec, 1, x->bounce);
        }
        if (r) return r;
    }
    x->bm_lo = ~0u; x->bm_hi = 0;
    return 0;
}
/* a free cluster, near want if it can be (0: the hint) */
static int alloc_clus(struct xfat *x, uint32_t want, uint32_t *out)
{
    if (!x->nfree) return FFS_FULL;
    uint32_t c = clus_ok(x, want) ? want : (clus_ok(x, x->hint) ? x->hint : 2);
    for (uint32_t n = 0; n < x->clusters; n++, c++) {
        if (!clus_ok(x, c)) c = 2;
        if (!bm_get(x, c)) { bm_set(x, c, 1); x->hint = c + 1; *out = c; return 0; }
    }
    return FFS_FULL;
}
/* new clusters' contents: zeros (for directories) */
static int zero_clus(struct xfat *x, uint32_t c)
{
    zero(x->bounce, 512);
    for (uint32_t k = 0; k < (1u << x->spc_shift); k++) { int r = wsec(x, clus_sec(x, c) + k, 1, x->bounce); if (r) return r; }
    return 0;
}

/* the up-case table for U+0000..U+00FF (Latin-1, what Amiga names hold) */
static int up_load(struct xfat *x)
{
    for (int i = 0; i < 256; i++) x->up[i] = (uint16_t)(i >= 'a' && i <= 'z' ? i - 32 : i);
    if (!x->up_first) return FFS_CORRUPT;
    uint32_t c = x->up_first, spc = 1u << x->spc_shift, pos = 0, ident = 0, want_count = 0;
    uint64_t left = x->up_len;
    while (pos < 256 && left >= 2 && clus_ok(x, c)) {
        for (uint32_t k = 0; k < spc && pos < 256 && left >= 2; k++) {
            if (x->io->read(x->io->ctx, clus_sec(x, c) + k, 1, x->bounce)) return FFS_IO;
            for (uint32_t i = 0; i < 512 && pos < 256 && left >= 2; i += 2, left -= 2) {
                uint32_t v = le16(x->bounce + i);
                if (want_count) { ident = v; want_count = 0; while (ident-- && pos < 256) { x->up[pos] = (uint16_t)pos; pos++; } }
                else if (v == 0xffff) want_count = 1;           /* a run of characters that are their own upper case */
                else x->up[pos++] = (uint16_t)v;
            }
        }
        c = fat_next(x, c);
    }
    return 0;
}

const char *xfat_writable(struct xfat *x)
{
    if (x->ro) return x->ro;
    if (!x->io->write) return x->ro = "the card is read-only";
    if (up_load(x)) return x->ro = "no up-case table";
    if (bm_load(x)) return x->ro = "the allocation bitmap cannot be read";
    if (x->flags & 2) {                                          /* not cleanly removed: the validator first */
        struct xchk r;
        int e = xfat_check(x, 1, &r);
        if (e) return x->ro = r.bad ? r.bad : "the check could not finish";
        hdf_log(x->io, "exFAT: not cleanly removed: checked %lu folders, %lu files; fixed %lu checksums, %lu entries, %lu chains, %lu sizes, %lu clusters freed, %lu marked",
                r.dirs, r.files, r.sums, r.dropped, r.trimmed, r.shortened, r.freed, r.marked);
        x->flags &= (uint16_t)~2u;
    }
    return 0;
}

static uint16_t name_hash(const struct xfat *x, const char *name, uint32_t len)
{
    uint16_t h = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t u = x->up[(uint8_t)name[i]];
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (u & 0xff));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (u >> 8));
    }
    return h;
}
static int name_ok(const char *n, uint32_t len)
{
    if (!len || len > FFS_NAME - 1) return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)n[i];
        if (c < 32 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') return 0;
    }
    return 1;
}

/* entries [at, at + n) of a directory: read into buf / written from buf */
static int get_ents(struct xfat *x, const struct xfat_dir *d, uint32_t at, uint8_t *buf, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *p;
        int r = entry(x, d, at + k, &p);
        if (r) return xerr(r == -1 ? -3 : r);
        copy(buf + 32 * k, p, 32);
    }
    return 0;
}
/* where entry idx of a directory is on the card (no sector read); 0 ok */
static int ent_loc(struct xfat *x, const struct xfat_dir *d, uint32_t idx, uint32_t *sec, uint32_t *off)
{
    uint64_t o = (uint64_t)idx * 32;
    uint32_t cshift = 9 + x->spc_shift;
    if (d->size && o >= d->size) return FFS_CORRUPT;
    uint32_t c = dir_clus(x, d, (uint32_t)(o >> cshift));
    if (!c) return FFS_CORRUPT;
    uint32_t within = (uint32_t)(o & ((1u << cshift) - 1));
    *sec = clus_sec(x, c) + within / 512;
    *off = within % 512;
    return 0;
}
static int put_ents(struct xfat *x, const struct xfat_dir *d, uint32_t at, const uint8_t *buf, uint32_t n)
{
    uint32_t cur = ~0u;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t sec, off;
        int r = ent_loc(x, d, at + k, &sec, &off);
        if (r) return r;
        if (sec != cur) {
            if (cur != ~0u && (r = wsec(x, cur, 1, x->sec))) return r;
            if ((r = read_sec(x, sec))) return xerr(r);
            cur = sec;
        }
        copy(x->sec + off, buf + 32 * k, 32);
    }
    return cur != ~0u ? wsec(x, cur, 1, x->sec) : 0;
}
static void set_sum(uint8_t *buf, uint32_t n)
{
    uint16_t s = 0;
    for (uint32_t k = 0; k < n; k++)
        for (int i = 0; i < 32; i++) {
            if (k == 0 && (i == 2 || i == 3)) continue;
            s = (uint16_t)(((s & 1) ? 0x8000 : 0) + (s >> 1) + buf[32 * k + i]);
        }
    put16(buf + 2, s);
}
/* an entry set as it is on the card (n entries); 0 ok */
static int read_set(struct xfat *x, uint32_t dirkey, uint32_t pos, uint8_t *buf, uint32_t *n)
{
    struct xfat_dir d;
    int r = xd_dir(x, dirkey, &d);
    if (r) return r;
    if ((r = get_ents(x, &d, pos, buf, 1))) return r;
    if (buf[0] != T_FILE || buf[1] < 2 || buf[1] > 18) return FFS_CORRUPT;
    *n = 1u + buf[1];
    if ((r = get_ents(x, &d, pos + 1, buf + 32, *n - 1))) return r;
    return buf[32] == T_STREAM ? 0 : FFS_CORRUPT;
}
/* write a changed set: what it points to (bitmap, FAT) goes to the card first */
static int write_set(struct xfat *x, uint32_t dirkey, uint32_t pos, uint8_t *buf, uint32_t n)
{
    struct xfat_dir d;
    int r = xd_dir(x, dirkey, &d);
    if (r) return r;
    set_sum(buf, n);
    if ((r = bm_flush(x)) || (r = fwin_flush(x)) || (r = set_dirty(x, 1))) return r;
    return put_ents(x, &d, pos, buf, n);
}

/* the last of k clusters from first (k >= 1) */
static uint32_t last_clus(struct xfat *x, uint32_t first, uint32_t k, int nofat)
{
    if (nofat) return first + k - 1;
    uint32_t c = first;
    for (uint32_t i = 1; i < k && c; i++) c = fat_next(x, c);
    return c;
}
/* grow a chain of k clusters (0: none yet) by one; *nofat and *first follow */
static int grow_chain(struct xfat *x, uint32_t *first, uint32_t k, int *nofat, uint32_t *added)
{
    uint32_t c;
    int r;
    if (!k) {
        if ((r = alloc_clus(x, 0, &c))) return r;
        *first = c; *nofat = 1; *added = c;
        return 0;
    }
    uint32_t last = last_clus(x, *first, k, *nofat);
    if (!clus_ok(x, last)) return FFS_CORRUPT;
    if ((r = alloc_clus(x, last + 1, &c))) return r;
    if (*nofat && c != last + 1) {                               /* not next to it: a FAT chain from now on */
        for (uint32_t i = 0; i + 1 < k; i++) if ((r = fat_set(x, *first + i, *first + i + 1))) return r;
        *nofat = 0;
    }
    if (!*nofat) {
        if ((r = fat_set(x, last, c)) || (r = fat_set(x, c, EOC))) return r;
    }
    *added = c;
    return 0;
}

/* a directory one cluster longer (its new entries zero) */
static int grow_dir(struct ffs *v, uint32_t key)
{
    struct xfat *x = v->x;
    struct xdir *k = xd_get(x, key);
    if (!k) return FFS_CORRUPT;
    uint32_t first = key, nk, c;
    int nofat = key == x->root ? 0 : (int)k->nofat, r;
    if (key == x->root) { nk = 1; for (uint32_t q = fat_next(x, key); q; q = fat_next(x, q)) nk++; }
    else nk = nclus(x, k->size);
    if ((r = grow_chain(x, &first, nk, &nofat, &c))) return r;
    if ((r = zero_clus(x, c))) return r;
    if (key == x->root) return 0;
    /* the directory's own entry in its parent: its new size */
    uint8_t set[19 * 32];
    uint32_t n;
    if ((r = read_set(x, k->parent, k->pos, set, &n))) return r;
    uint64_t size = (uint64_t)(nk + 1) * cbytes(x);
    set[32 + 1] = (uint8_t)(1 | (nofat ? 2 : 0));
    put64(set + 32 + 8, size); put64(set + 32 + 24, size);
    if ((r = write_set(x, k->parent, k->pos, set, n))) return r;
    k->size = size; k->nofat = (uint32_t)nofat;
    x->wc_first = 0;
    return 0;
}

/* need free entries in a row in a directory (it grows if it must): where */
static int find_slots(struct ffs *v, uint32_t key, uint32_t need, uint32_t *at)
{
    struct xfat *x = v->x;
    for (int grown = 0; grown < 4; grown++) {
        struct xfat_dir d;
        int r = xd_dir(x, key, &d);
        if (r) return r;
        uint32_t run = 0, start = 0;
        for (uint32_t idx = 0; ; idx++) {
            const uint8_t *p;
            r = entry(x, &d, idx, &p);
            if (r == -1) break;
            if (r) return xerr(r);
            if (p[0] & 0x80) { run = 0; continue; }
            if (!run) start = idx;
            if (++run == need) { *at = start; return 0; }
        }
        if ((r = grow_dir(v, key))) return r;
    }
    return FFS_DIRFULL;
}

static void stamp(uint8_t *f, uint32_t ts, int created)
{
    if (created) put32(f + 8, ts);
    put32(f + 12, ts); put32(f + 16, ts);
    if (created) f[20] = 0;
    f[21] = 0; f[22] = f[23] = f[24] = 0;                       /* no UTC offset known */
}
static void put_name(const struct xfat *x, uint8_t *set, const char *name, uint32_t len)
{
    set[32 + 3] = (uint8_t)len;
    put16(set + 32 + 4, name_hash(x, name, len));
    for (uint32_t k = 0; k * 15 < len; k++) {
        uint8_t *e = set + 64 + 32 * k;
        zero(e, 32);
        e[0] = T_NAME;
        for (uint32_t i = 0; i < 15 && k * 15 + i < len; i++) put16(e + 2 + 2 * i, (uint8_t)name[k * 15 + i]);
    }
}

static int must_write(struct ffs *v) { return v->x->ro ? FFS_WP : 0; }

int xfs_create(struct ffs *v, uint32_t dir, const char *name, uint32_t len, int isdir, struct ffs_ent *o)
{
    struct xfat *x = v->x;
    int r;
    if ((r = must_write(v))) return r;
    if (!name_ok(name, len)) return FFS_BADNAME;
    struct ffs_ent e;
    if ((r = xfs_find(v, dir, name, len, &e)) != FFS_END) return r ? r : FFS_EXISTS;
    uint32_t nn = (len + 14) / 15, n = 2 + nn, at, c = 0;
    if (isdir) {                                                 /* its first cluster, empty, before the entry */
        if ((r = alloc_clus(x, 0, &c)) || (r = zero_clus(x, c))) return r;
    }
    if ((r = find_slots(v, dir, n, &at))) { if (c) bm_set(x, c, 0); return r; }
    uint8_t set[19 * 32];
    zero(set, sizeof set);
    set[0] = T_FILE; set[1] = (uint8_t)(n - 1);
    put16(set + 4, isdir ? XFAT_A_DIR : XFAT_A_ARCH);
    stamp(set, ((uint32_t)v->date << 16) | v->time, 1);
    set[32] = T_STREAM;
    set[32 + 1] = (uint8_t)(isdir ? 3 : 1);                      /* AllocationPossible (+ NoFatChain) */
    if (isdir) { put32(set + 32 + 20, c); put64(set + 32 + 8, cbytes(x)); put64(set + 32 + 24, cbytes(x)); }
    put_name(x, set, name, len);
    if ((r = write_set(x, dir, at, set, n))) return r;
    uint32_t pos = at;
    return xfs_next(v, dir, &pos, o);
}

int xfs_delete(struct ffs *v, const struct ffs_ent *e)
{
    struct xfat *x = v->x;
    int r;
    if ((r = must_write(v))) return r;
    if (e->attr & FFS_A_DIR) {
        uint32_t pos = 0;
        struct ffs_ent c;
        if ((r = xfs_next(v, e->first, &pos, &c)) != FFS_END) return r ? r : FFS_NOTEMPTY;
    }
    uint8_t set[19 * 32];
    uint32_t n;
    if ((r = read_set(x, e->dir, e->idx, set, &n))) return r;
    if (le32(set + 32 + 20) != e->first) return FFS_CORRUPT;
    struct xfat_dir d;
    if ((r = xd_dir(x, e->dir, &d))) return r;
    for (uint32_t k = 0; k < n; k++) set[32 * k] &= 0x7f;      /* not in use */
    if ((r = set_dirty(x, 1)) || (r = put_ents(x, &d, e->idx, set, n))) return r;
    /* then its clusters are free */
    if (e->first && (set[32 + 1] & 1)) {
        uint32_t k = nclus(x, e->xsize), c = e->first;
        for (uint32_t i = 0; i < k && clus_ok(x, c); i++) {
            bm_set(x, c, 0);
            c = e->xnofat ? c + 1 : fat_next(x, c);
        }
    }
    return 0;
}

int xfs_rename(struct ffs *v, const struct ffs_ent *e, uint32_t ndir, const char *name, uint32_t len, struct ffs_ent *o)
{
    struct xfat *x = v->x;
    int r;
    if ((r = must_write(v))) return r;
    if (!name_ok(name, len)) return FFS_BADNAME;
    if (e->attr & FFS_A_DIR)                                     /* not into itself */
        for (uint32_t d = ndir, g = 0; g < 10000; g++) {
            if (d == e->first) return FFS_LOOP;
            uint32_t p;
            if (d == x->root || xfs_parent(v, d, &p)) break;
            d = p;
        }
    struct ffs_ent t;
    if ((r = xfs_find(v, ndir, name, len, &t)) == 0 && !(t.dir == e->dir && t.idx == e->idx)) return FFS_EXISTS;
    if (r && r != FFS_END) return r;
    uint8_t set[19 * 32];
    uint32_t n;
    if ((r = read_set(x, e->dir, e->idx, set, &n))) return r;
    uint32_t nn = (len + 14) / 15, nn_ = 2 + nn;
    struct xfat_dir od;
    if ((r = xd_dir(x, e->dir, &od))) return r;
    if (ndir == e->dir && nn_ <= n) {                            /* in place */
        uint8_t old[19 * 32];
        copy(old, set, n * 32);
        set[1] = (uint8_t)(nn_ - 1);
        put_name(x, set, name, len);
        for (uint32_t k = nn_; k < n; k++) { copy(set + 32 * k, old + 32 * k, 32); set[32 * k] &= 0x7f; }
        set_sum(set, nn_);
        if ((r = bm_flush(x)) || (r = fwin_flush(x)) || (r = set_dirty(x, 1)) || (r = put_ents(x, &od, e->idx, set, n))) return r;
        uint32_t pos = e->idx;
        return xfs_next(v, ndir, &pos, o);
    }
    /* a new set where it goes, then the old one let go */
    uint32_t at;
    if ((r = find_slots(v, ndir, nn_, &at))) return r;
    if (ndir == e->dir && (r = read_set(x, e->dir, e->idx, set, &n))) return r;   /* the directory may have grown */
    uint8_t ns[19 * 32];
    zero(ns, sizeof ns);
    copy(ns, set, 64);
    ns[1] = (uint8_t)(nn_ - 1);
    put_name(x, ns, name, len);
    if ((r = write_set(x, ndir, at, ns, nn_))) return r;
    for (uint32_t k = 0; k < n; k++) set[32 * k] &= 0x7f;
    if ((r = put_ents(x, &od, e->idx, set, n))) return r;
    uint32_t pos = at;
    return xfs_next(v, ndir, &pos, o);
}

static int edit_file_entry(struct ffs *v, struct ffs_ent *e, int attr, uint32_t ts)
{
    struct xfat *x = v->x;
    int r;
    if ((r = must_write(v))) return r;
    uint8_t set[19 * 32];
    uint32_t n;
    if ((r = read_set(x, e->dir, e->idx, set, &n))) return r;
    if (attr >= 0) put16(set + 4, (le16(set + 4) & XFAT_A_DIR) | ((uint32_t)attr & ~(uint32_t)XFAT_A_DIR & 0x37));
    else { put32(set + 12, ts); set[21] = 0; set[23] = 0; }
    if ((r = write_set(x, e->dir, e->idx, set, n))) return r;
    if (attr >= 0) e->attr = (uint8_t)le16(set + 4);
    else { e->date = (uint16_t)(ts >> 16); e->time = (uint16_t)ts; }
    return 0;
}
int xfs_set_attr(struct ffs *v, struct ffs_ent *e, uint8_t attr) { return edit_file_entry(v, e, attr, 0); }
int xfs_set_date(struct ffs *v, struct ffs_ent *e, uint16_t date, uint16_t time) { return edit_file_entry(v, e, -1, ((uint32_t)date << 16) | time); }

int xfs_flush_file(struct ffs *v, struct ffs_file *h)
{
    struct xfat *x = v->x;
    if (!h->dirty) return 0;
    int r;
    if ((r = must_write(v))) return r;
    uint8_t set[19 * 32];
    uint32_t n;
    if ((r = read_set(x, h->e.dir, h->e.idx, set, &n))) return r;
    uint8_t *st = set + 32;
    st[1] = (uint8_t)(1 | (h->e.first && h->e.xnofat ? 2 : 0));
    put32(st + 20, h->e.first);
    put64(st + 8, h->e.xvalid);
    put64(st + 24, h->e.xsize);
    put16(set + 4, le16(set + 4) | XFAT_A_ARCH);
    stamp(set, ((uint32_t)v->date << 16) | v->time, 0);
    if ((r = write_set(x, h->e.dir, h->e.idx, set, n))) return r;
    h->e.attr = (uint8_t)le16(set + 4);
    h->e.date = v->date; h->e.time = v->time;
    h->dirty = 0;
    return 0;
}

/* clusters for the file's first `need` (its DataLength follows separately) */
static int ensure(struct xfat *x, struct ffs_file *h, uint32_t need)
{
    uint32_t k = h->e.first ? nclus(x, h->e.xsize) : 0;
    if (h->e.first && !k) k = 1;
    int nofat = h->e.first ? h->e.xnofat : 1;
    while (k < need) {
        uint32_t c, first = h->e.first;
        int r = grow_chain(x, &first, k, &nofat, &c);
        if (r) return r;
        if (!h->e.first) h->e.first = first;
        h->e.xnofat = (uint8_t)nofat;
        k++;
        h->e.xsize = (uint64_t)k * cbytes(x) < h->e.xsize ? h->e.xsize : (uint64_t)(k - 1) * cbytes(x) + 1;
        h->dirty = 1;
    }
    h->ci = ~0u;
    return 0;
}

static int put_bytes(struct ffs *v, struct ffs_file *h, uint32_t pos, const uint8_t *p, uint32_t n)
{
    struct xfat *x = v->x;
    uint32_t cshift = 9 + x->spc_shift;
    while (n) {
        uint32_t c = file_clus(x, h, pos >> cshift);
        if (!c) return FFS_CORRUPT;
        uint32_t in = pos & ((1u << cshift) - 1), sec = clus_sec(x, c) + in / 512, off = pos & 511;
        uint32_t avail = (1u << cshift) - in;
        if (avail > n) avail = n;
        int r;
        if (!off && avail >= 512 && p && !((uintptr_t)p & 3)) {
            avail &= ~511u;
            r = wsec(x, sec, avail / 512, p);
        } else {
            if (avail > 512 - off) avail = 512 - off;
            if (off || avail < 512) { if (x->io->read(x->io->ctx, sec, 1, x->bounce)) return werr(x, -1); }
            if (p) copy(x->bounce + off, p, avail); else zero(x->bounce + off, avail);
            r = wsec(x, sec, 1, x->bounce);
        }
        if (r) return r;
        if (p) p += avail;
        pos += avail; n -= avail;
    }
    return 0;
}

int32_t xfs_write(struct ffs *v, struct ffs_file *h, uint32_t pos, const void *buf, uint32_t n, int *err)
{
    struct xfat *x = v->x;
    *err = 0;
    int r;
    if ((r = must_write(v))) { *err = r; return 0; }
    if (!n) return 0;
    if (pos > 0xffffffffu - n) n = 0xffffffffu - pos;           /* AmigaDOS: 32-bit sizes */
    uint64_t end = (uint64_t)pos + n;
    uint64_t oldsize = h->e.xsize;
    if ((r = ensure(x, h, nclus(x, end)))) {                     /* a full card: as much as fits */
        uint64_t room = (uint64_t)(h->e.first ? nclus(x, h->e.xsize) : 0) * cbytes(x);
        *err = r;
        if (room <= pos) { h->e.xsize = oldsize > room ? room : oldsize; return 0; }
        n = (uint32_t)(room - pos); end = room;
    }
    if (pos > h->e.xvalid && (r = put_bytes(v, h, (uint32_t)h->e.xvalid, 0, pos - (uint32_t)h->e.xvalid))) { *err = r; return 0; }
    if ((r = put_bytes(v, h, pos, buf, n))) { *err = r; return 0; }
    h->e.xsize = end > oldsize ? end : oldsize;
    if (end > h->e.xvalid) h->e.xvalid = end;
    h->e.size = (uint32_t)h->e.xsize;
    h->dirty = 1;
    return (int32_t)n;
}

int xfs_truncate(struct ffs *v, struct ffs_file *h, uint32_t size)
{
    struct xfat *x = v->x;
    int r;
    if ((r = must_write(v))) return r;
    uint32_t newk = nclus(x, size), k = h->e.first ? nclus(x, h->e.xsize) : 0;
    if (h->e.first && !k) k = 1;
    if (newk > k) {
        if ((r = ensure(x, h, newk))) return r;
        h->e.xsize = size; h->e.size = size;
        h->dirty = 1;
        return 0;
    }
    /* smaller: the entry first, then the clusters it no longer has */
    uint32_t first = h->e.first, nofat = h->e.xnofat;
    h->e.xsize = size; h->e.size = size;
    if (h->e.xvalid > size) h->e.xvalid = size;
    if (!newk) { h->e.first = 0; h->e.xnofat = 0; }
    h->dirty = 1;
    if ((r = xfs_flush_file(v, h))) return r;
    uint32_t c = first;
    for (uint32_t i = 0; i < k && clus_ok(x, c); i++) {
        uint32_t next = nofat ? c + 1 : fat_next(x, c);
        if (i + 1 == newk && !nofat) { if ((r = fat_set(x, c, EOC))) return r; }
        if (i >= newk) bm_set(x, c, 0);
        c = next;
    }
    h->ci = ~0u;
    return 0;
}

int xfs_sync(struct ffs *v)
{
    struct xfat *x = v->x;
    if (!x->ro) { bm_flush(x); fwin_flush(x); if (!x->err) set_dirty(x, 0); }
    int e = x->err ? x->err : v->err;
    x->err = 0; v->err = 0;
    return e;
}

/* ---- the validator (xfat_check) ------------------------------------------------------------- */
struct chk {
    struct xfat *x;
    struct xchk *r;
    int repair;
    uint8_t *used;                  /* clusters something refers to */
    struct xfat_dir *stack;         /* directories still to walk */
    uint32_t nstack, capstack;
};

/* 0 marked, 1 someone has it already */
static int chk_mark(struct chk *k, uint32_t c)
{
    uint32_t i = c - 2;
    uint8_t m = (uint8_t)(1u << (i & 7));
    if (k->used[i >> 3] & m) return 1;
    k->used[i >> 3] |= m;
    return 0;
}

/* the clusters of a chain (want: how many, ~0 = to its end); *got how many it has.
   0 ok; 1 the first cluster is someone else's (nothing marked); -1 it crosses into
   someone else's later, or runs outside the heap, or loops */
static int chk_chain(struct chk *k, uint32_t first, uint32_t want, int nofat, uint32_t *got, uint32_t *last)
{
    struct xfat *x = k->x;
    uint32_t n = 0, c = first;
    *got = 0; *last = 0;
    if (!clus_ok(x, first)) return -1;
    if (nofat && want != ~0u && (uint64_t)first - 2 + want > x->clusters) return -1;
    while (want == ~0u || n < want) {
        if (!clus_ok(x, c)) break;                               /* a FAT chain ending early */
        if (chk_mark(k, c)) {
            if (!n) return 1;
            return -1;
        }
        n++; *last = c;
        if (n > x->clusters) return -1;
        if (want != ~0u && n == want) break;
        c = nofat ? c + 1 : fat_next(x, c);
    }
    *got = n;
    return 0;
}

static int chk_push(struct chk *k, const struct xfat_dir *d)
{
    if (k->nstack == k->capstack) {
        uint32_t cap = k->capstack ? k->capstack * 2 : 32;
        struct xfat_dir *n = k->x->io->alloc(k->x->io->ctx, cap * sizeof *n);
        if (!n) return FFS_IO;
        if (k->stack) { copy(n, k->stack, k->nstack * sizeof *n); k->x->io->free(k->x->io->ctx, k->stack, k->capstack * sizeof *n); }
        k->stack = n; k->capstack = cap;
    }
    k->stack[k->nstack++] = *d;
    return 0;
}

/* drop entries [at, at + n): not in use */
static int chk_drop(struct chk *k, const struct xfat_dir *d, uint32_t at, uint8_t *buf, uint32_t n)
{
    k->r->dropped++;
    if (!k->repair) return 0;
    for (uint32_t i = 0; i < n; i++) buf[32 * i] &= 0x7f;
    return put_ents(k->x, d, at, buf, n);
}

static int chk_dir(struct chk *k, const struct xfat_dir *d)
{
    struct xfat *x = k->x;
    uint8_t set[19 * 32];
    for (uint32_t idx = 0; ; ) {
        const uint8_t *p;
        int r = entry(x, d, idx, &p);
        if (r == -1) return 0;
        if (r) return FFS_IO;
        uint8_t t = p[0];
        if (t == T_END) return 0;
        if (!(t & 0x80)) { idx++; continue; }                  /* not in use */
        if (t == T_STREAM || t == T_NAME) {                      /* a piece of a set without its File entry */
            copy(set, p, 32);
            if ((r = chk_drop(k, d, idx, set, 1))) return r;
            idx++; continue;
        }
        if (t != T_FILE) { idx++; continue; }                    /* bitmap, up-case table, label, GUID, vendor */
        uint32_t n = 1u + p[1];
        if (n < 3 || n > 19) { copy(set, p, 32); if ((r = chk_drop(k, d, idx, set, 1))) return r; idx++; continue; }
        if ((r = get_ents(x, d, idx, set, n))) { if (r != FFS_CORRUPT) return r; copy(set, p, 32); if ((r = chk_drop(k, d, idx, set, 1))) return r; idx++; continue; }
        uint8_t *st = set + 32;
        uint32_t namelen = st[3], names = 0;
        for (uint32_t i = 2; i < n && set[32 * i] == T_NAME; i++) names++;
        if (st[0] != T_STREAM || !namelen || names < (namelen + 14) / 15) {
            if ((r = chk_drop(k, d, idx, set, st[0] == T_STREAM ? 2 + names : 1))) return r;
            idx += st[0] == T_STREAM ? 2 + names : 1;
            continue;
        }
        int changed = 0;
        uint16_t want = (uint16_t)le16(set + 2);
        set_sum(set, n);
        if ((uint16_t)le16(set + 2) != want) { k->r->sums++; changed = 1; }
        int isdir = (le16(set + 4) & XFAT_A_DIR) != 0;
        uint32_t first = le32(st + 20);
        uint64_t size = le64(st + 24);
        int nofat = (st[1] & 2) != 0;
        if ((st[1] & 1) && first) {
            uint32_t need = nclus(x, size), got, last;
            if (!need) need = 1;
            int c = chk_chain(k, first, need, nofat, &got, &last);
            if (c == 1) {                                        /* a duplicate (a move cut off): this one goes */
                if ((r = chk_drop(k, d, idx, set, n))) return r;
                idx += n;
                continue;
            }
            if (c < 0) { k->r->bad = "two files share clusters, or a chain leaves the volume: check it on a PC"; return FFS_CORRUPT; }
            if (got < need) {                                    /* the chain is shorter than the file */
                k->r->shortened++;
                uint64_t ns = (uint64_t)got * cbytes(x);
                put64(st + 24, ns);
                if (le64(st + 8) > ns) put64(st + 8, ns);
                size = ns;
                set_sum(set, n);
                changed = 1;
            } else if (!nofat && clus_ok(x, fat_next(x, last))) { /* longer than the file: the rest is let go */
                k->r->trimmed++;
                if (k->repair && (r = fat_set(x, last, EOC))) return r;
            }
            if (isdir) {
                struct xfat_dir sub = { first, size, nofat };
                if (size && (r = chk_push(k, &sub))) return r;
            }
        }
        if (changed && k->repair && (r = put_ents(x, d, idx, set, n))) return r;
        if (isdir) k->r->dirs++; else k->r->files++;
        idx += n;
    }
}

static int chk_pass(struct chk *k)
{
    struct xfat *x = k->x;
    uint32_t got, last;
    int r;
    zero(k->used, (uint32_t)x->bm_len);
    k->nstack = 0;
    /* the volume's own structures */
    if (chk_chain(k, x->bm_first, nclus(x, x->bm_len), 0, &got, &last) || got < nclus(x, x->bm_len)) { k->r->bad = "the allocation bitmap's clusters are damaged"; return FFS_CORRUPT; }
    if (x->up_first && (chk_chain(k, x->up_first, nclus(x, x->up_len), 0, &got, &last) || got < nclus(x, x->up_len))) { k->r->bad = "the up-case table's clusters are damaged"; return FFS_CORRUPT; }
    if (chk_chain(k, x->root, ~0u, 0, &got, &last)) { k->r->bad = "the root directory's clusters are damaged"; return FFS_CORRUPT; }
    struct xfat_dir root;
    xfat_root(x, &root);
    if ((r = chk_push(k, &root))) return r;
    while (k->nstack) {
        struct xfat_dir d = k->stack[--k->nstack];
        x->wc_first = 0;
        if ((r = chk_dir(k, &d))) return r;
    }
    /* what the bitmap says against what is used */
    for (uint32_t c = 2; c < x->clusters + 2; c++) {
        uint32_t i = c - 2;
        int u = (k->used[i >> 3] >> (i & 7)) & 1, b = bm_get(x, c);
        if (u == b) continue;
        if (b) k->r->freed++; else k->r->marked++;
        if (k->repair) bm_set(x, c, u);
    }
    return 0;
}

int xfat_check(struct xfat *x, int repair, struct xchk *r)
{
    zero(r, sizeof *r);
    struct chk k;
    zero(&k, sizeof k);
    k.x = x; k.r = r;
    int e = bm_load(x);
    if (e) { r->bad = "the allocation bitmap cannot be read"; return e; }
    if (!x->up_first || up_load(x)) { r->bad = "no up-case table"; return FFS_CORRUPT; }
    k.used = x->io->alloc(x->io->ctx, (uint32_t)x->bm_len);
    if (!k.used) { r->bad = "not enough memory to check it"; return FFS_IO; }
    e = chk_pass(&k);                                            /* pass 1: look */
    if (!e && repair && (r->sums | r->dropped | r->trimmed | r->shortened | r->freed | r->marked)) {
        struct xchk seen = *r;
        zero(r, sizeof *r);
        k.repair = 1;
        x->dirty_on_card = 1;                                    /* it is: set by whoever was cut off */
        e = chk_pass(&k);                                        /* pass 2: the same, fixing */
        if (!e && ((e = fwin_flush(x)) == 0)) e = bm_flush(x);
        if (!e) *r = seen;
    }
    if (!e && repair) { x->dirty_on_card = 1; e = set_dirty(x, 0); }
    if (k.stack) x->io->free(x->io->ctx, k.stack, k.capstack * sizeof *k.stack);
    x->io->free(x->io->ctx, k.used, (uint32_t)x->bm_len);
    x->sec_no = ~0u; x->wc_first = 0;
    return e;
}
