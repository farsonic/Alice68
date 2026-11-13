/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68 folders: FAT16/FAT32 reading and writing - see fatfs.h. */
#include "fatfs.h"
#include "exfat.h"

#define EPS      16u                            /* entries per sector */
#define EOC32    0x0fffffffu
#define EOC16    0xffffu

static void zero(void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static uint32_t clus_sec(const struct fat *f, uint32_t c) { return f->data_start + (c - 2) * f->spc; }
static uint32_t ent_first(const struct fat *f, const uint8_t *e) { return le16(e + 26) | (f->fat32 ? le16(e + 20) << 16 : 0); }
static void set_first(const struct fat *f, uint8_t *e, uint32_t c) { put16(e + 26, c & 0xffff); put16(e + 20, f->fat32 ? c >> 16 : 0); }
uint32_t ffs_cluster_bytes(const struct ffs *v) { return v->x ? xfs_cluster_bytes(v) : v->f->spc * HDF_BLOCK; }
uint32_t ffs_clusters(const struct ffs *v) { return v->x ? xfs_clusters(v) : v->f->clusters; }
uint32_t ffs_clus_sec(const struct ffs *v, uint32_t c) { return v->x ? xfs_clus_sec(v, c) : clus_sec(v->f, c); }
void ffs_set_io(struct ffs *v, struct hdf_io *io) { if (v->x) xfs_set_io(v, io); else v->f->io = io; }

static int io_err(struct ffs *v, int r)
{
    int e = r == -2 ? FFS_WP : FFS_IO;
    if (!v->err) v->err = e;
    return e;
}
static int rd(struct ffs *v, uint32_t sec, uint32_t n, void *buf)
{
    int r = v->f->io->read(v->f->io->ctx, sec, n, buf);
    return r ? io_err(v, r) : 0;
}
static int wr(struct ffs *v, uint32_t sec, uint32_t n, const void *buf)
{
    if (!v->f->io->write) return io_err(v, -2);
    if (v->dsec_no != ~0u && v->dsec_no - sec < n) { v->dsec_no = ~0u; v->dsec_dirty = 0; }   /* newer than the cached copy */
    int r = v->f->io->write(v->f->io->ctx, sec, n, buf);
    return r ? io_err(v, r) : 0;
}

int ffs_init(struct ffs *v, struct fat *f)
{
    struct hdf_io *io = f->io;
    zero(v, sizeof *v);
    v->f = f;
    v->dsec_no = v->fwin_first = v->dc_dir = v->nfree = v->fsinfo_free = ~0u;
    v->hint = 2;
    v->dsec = io->alloc(io->ctx, HDF_BLOCK);
    v->bounce = io->alloc(io->ctx, HDF_BLOCK);
    v->fwin = io->alloc(io->ctx, FFS_FATWIN * HDF_BLOCK);
    if (!v->dsec || !v->bounce || !v->fwin) { ffs_done(v); return FFS_IO; }
    v->date = (uint16_t)((46 << 9) | (1 << 5) | 1);          /* 2026-01-01 until the caller says */
    return 0;
}

void ffs_done(struct ffs *v)
{
    if (v->x) { xfs_done(v); return; }
    struct hdf_io *io = v->f->io;
    if (v->dsec) io->free(io->ctx, v->dsec, HDF_BLOCK);
    if (v->bounce) io->free(io->ctx, v->bounce, HDF_BLOCK);
    if (v->fwin) io->free(io->ctx, v->fwin, FFS_FATWIN * HDF_BLOCK);
    v->dsec = v->bounce = v->fwin = 0;
}

/* ---- the FAT ------------------------------------------------------------ */
static int fwin_flush(struct ffs *v)
{
    struct fat *f = v->f;
    for (uint32_t s = 0; v->fwin_dirty; s++) {
        if (!(v->fwin_dirty & (1u << s))) continue;
        for (uint32_t k = 0; k < f->nfats; k++)               /* every copy */
            if (wr(v, f->fat_start + k * f->fat_secs + (v->fwin_first - f->fat_start) + s, 1, v->fwin + s * HDF_BLOCK)) return v->err;
        v->fwin_dirty &= ~(1u << s);
    }
    return 0;
}

static uint8_t *fat_ptr(struct ffs *v, uint32_t c)
{
    struct fat *f = v->f;
    uint32_t off = f->fat32 ? c * 4 : c * 2, sec = f->fat_start + off / HDF_BLOCK;
    if (v->fwin_first == ~0u || sec < v->fwin_first || sec >= v->fwin_first + FFS_FATWIN) {
        if (fwin_flush(v)) return 0;
        uint32_t first = sec, n = FFS_FATWIN;
        if (first - f->fat_start + n > f->fat_secs) n = f->fat_secs - (first - f->fat_start);
        if (rd(v, first, n, v->fwin)) { v->fwin_first = ~0u; return 0; }
        v->fwin_first = first;
    }
    return v->fwin + (sec - v->fwin_first) * HDF_BLOCK + off % HDF_BLOCK;
}

/* the raw entry; ~0 = unreadable */
static uint32_t fat_get(struct ffs *v, uint32_t c)
{
    uint8_t *p = fat_ptr(v, c);
    if (!p) return ~0u;
    return v->f->fat32 ? le32(p) & 0x0fffffffu : le16(p);
}

static int mark_fsinfo(struct ffs *v)
{
    if (v->fsinfo_done || !v->f->fsinfo) return 0;
    v->fsinfo_done = 1;                                       /* the free count is no longer right: say "unknown" */
    if (rd(v, v->f->fsinfo, 1, v->bounce)) return v->err;
    if (le32(v->bounce) != 0x41615252u || le32(v->bounce + 484) != 0x61417272u) return 0;
    put32(v->bounce + 488, 0xffffffffu);
    put32(v->bounce + 492, 0xffffffffu);
    return wr(v, v->f->fsinfo, 1, v->bounce);
}

static int fat_set(struct ffs *v, uint32_t c, uint32_t val)
{
    if (c < 2 || c >= v->f->clusters + 2) return FFS_CORRUPT;
    if (mark_fsinfo(v)) return v->err;
    uint8_t *p = fat_ptr(v, c);
    if (!p) return v->err ? v->err : FFS_IO;
    uint32_t old = v->f->fat32 ? le32(p) & 0x0fffffffu : le16(p);
    if (v->f->fat32) put32(p, (le32(p) & 0xf0000000u) | (val & 0x0fffffffu));
    else put16(p, val);
    uint32_t sec = (uint32_t)(p - v->fwin) / HDF_BLOCK;
    v->fwin_dirty |= 1u << sec;
    if (v->nfree != ~0u) {
        if (old == 0 && val) v->nfree--;
        else if (old && !val) v->nfree++;
    }
    return 0;
}

/* the next cluster of a chain: 0 = the end; ~0 = broken */
static uint32_t chain_next(struct ffs *v, uint32_t c)
{
    uint32_t n = fat_get(v, c);
    if (n == ~0u) return ~0u;
    if (v->f->fat32 ? n >= 0x0ffffff8u : n >= 0xfff8u) return 0;
    if (n < 2 || n >= v->f->clusters + 2) return ~0u;          /* free or bad inside a chain */
    return n;
}

static uint32_t eoc(const struct ffs *v) { return v->f->fat32 ? EOC32 : EOC16; }

/* a free cluster, marked end-of-chain (near "near" if it can be) */
static int alloc_clus(struct ffs *v, uint32_t near, uint32_t *out)
{
    uint32_t n = v->f->clusters, start = near >= 2 && near < n + 2 ? near : v->hint;
    if (start < 2 || start >= n + 2) start = 2;
    if (v->nfree == 0) return FFS_FULL;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = 2 + (start - 2 + i) % n;
        uint32_t x = fat_get(v, c);
        if (x == ~0u) return v->err ? v->err : FFS_IO;
        if (x == 0) {
            int r = fat_set(v, c, eoc(v));
            if (r) return r;
            v->hint = c + 1;
            *out = c;
            return 0;
        }
    }
    v->nfree = 0;
    return FFS_FULL;
}

static int free_chain(struct ffs *v, uint32_t c)
{
    uint32_t guard = v->f->clusters + 1;
    while (c >= 2 && guard--) {
        uint32_t n = chain_next(v, c);
        int r = fat_set(v, c, 0);
        if (r) return r;
        if (c < v->hint) v->hint = c;
        if (n == ~0u) return FFS_CORRUPT;
        c = n;
    }
    return 0;
}

int ffs_free_clusters(struct ffs *v, uint32_t *n)
{
    if (v->x) return xfs_free_clusters(v, n);
    if (v->nfree == ~0u) {
        uint32_t k = 0;
        for (uint32_t c = 2; c < v->f->clusters + 2; c++) {
            uint32_t x = fat_get(v, c);
            if (x == ~0u) return v->err ? v->err : FFS_IO;
            if (!x) k++;
        }
        v->nfree = k;
    }
    *n = v->nfree;
    return 0;
}

/* ---- directory sectors ---------------------------------------------------- */
static int dsec_flush(struct ffs *v)
{
    if (!v->dsec_dirty) return 0;
    if (wr(v, v->dsec_no, 1, v->dsec)) return v->err;
    v->dsec_dirty = 0;
    return 0;
}
static uint8_t *dsec_load(struct ffs *v, uint32_t sec)
{
    if (v->dsec_no == sec) return v->dsec;
    if (dsec_flush(v)) return 0;
    if (rd(v, sec, 1, v->dsec)) { v->dsec_no = ~0u; return 0; }
    v->dsec_no = sec;
    return v->dsec;
}

/* FSInfo: the free count, once it is known again */
static void fsinfo_update(struct ffs *v)
{
    if (!v->fsinfo_done || v->nfree == ~0u || v->nfree == v->fsinfo_free || v->err) return;
    if (rd(v, v->f->fsinfo, 1, v->bounce)) return;
    if (le32(v->bounce) != 0x41615252u || le32(v->bounce + 484) != 0x61417272u) return;
    put32(v->bounce + 488, v->nfree);
    put32(v->bounce + 492, v->hint < v->f->clusters + 2 ? v->hint : 2);
    if (!wr(v, v->f->fsinfo, 1, v->bounce)) v->fsinfo_free = v->nfree;
}

int ffs_sync(struct ffs *v)
{
    if (v->x) return xfs_sync(v);
    dsec_flush(v);
    fwin_flush(v);
    fsinfo_update(v);
    int e = v->err;
    v->err = 0;
    return e;
}

static int fixed_root(const struct ffs *v, uint32_t dir) { return !dir && !v->f->fat32; }
static uint32_t dir_first(const struct ffs *v, uint32_t dir) { return dir ? dir : v->f->root_clus; }

/* the card sector holding entry idx of a directory; FFS_END past its end */
static int dir_sec(struct ffs *v, uint32_t dir, uint32_t idx, uint32_t *sec)
{
    struct fat *f = v->f;
    if (fixed_root(v, dir)) {
        if (idx >= f->root_secs * EPS) return FFS_END;
        *sec = f->root_start + idx / EPS;
        return 0;
    }
    uint32_t epc = f->spc * EPS, ci = idx / epc, k, c;
    if (v->dc_dir == dir && v->dc_ci <= ci) { k = v->dc_ci; c = v->dc_clus; }
    else { k = 0; c = dir_first(v, dir); }
    while (k < ci) {
        uint32_t n = chain_next(v, c);
        if (n == ~0u) return v->err ? v->err : FFS_CORRUPT;
        if (!n) { v->dc_dir = dir; v->dc_ci = k; v->dc_clus = c; return FFS_END; }
        c = n; k++;
    }
    v->dc_dir = dir; v->dc_ci = ci; v->dc_clus = c;
    *sec = clus_sec(f, c) + (idx % epc) / EPS;
    return 0;
}

/* entry idx of a directory, in the directory sector buffer */
static int dir_ent(struct ffs *v, uint32_t dir, uint32_t idx, uint8_t **e, uint32_t *secp)
{
    uint32_t sec;
    int r = dir_sec(v, dir, idx, &sec);
    if (r) return r;
    uint8_t *s = dsec_load(v, sec);
    if (!s) return v->err;
    *e = s + 32 * (idx % EPS);
    if (secp) *secp = sec;
    return 0;
}

/* ---- names --------------------------------------------------------------------- */
static uint8_t lower(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) return (uint8_t)(c + 32);
    return c;
}
static uint8_t upper(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) return (uint8_t)(c - 32);
    return c;
}

int ffs_name_eq(const char *a, const char *b, uint32_t blen)
{
    uint32_t i = 0;
    for (; i < blen && a[i]; i++) if (lower((uint8_t)a[i]) != lower((uint8_t)b[i])) return 0;
    return i == blen && !a[i];
}

static uint8_t lfn_sum(const uint8_t *e)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

static void sfn_text(const uint8_t *e, char *out, int cased)
{
    unsigned k = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        uint8_t c = i == 0 && e[0] == 0x05 ? 0xe5 : e[i];
        out[k++] = (char)(cased && (e[12] & 0x08) ? lower(c) : c);
    }
    if (e[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[k++] = (char)(cased && (e[12] & 0x10) ? lower(e[i]) : e[i]);
    }
    out[k] = 0;
}

static const uint8_t lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static void fill_ent(struct ffs *v, struct ffs_ent *o, const uint8_t *e, uint32_t dir, uint32_t idx, uint32_t sec)
{
    o->dir = dir; o->idx = idx;
    o->sec = sec; o->slot = idx % EPS;
    o->first = ent_first(v->f, e);
    o->size = le32(e + 28);
    o->attr = e[11];
    o->time = (uint16_t)le16(e + 22);
    o->date = (uint16_t)le16(e + 24);
    sfn_text(e, o->sfn, 0);
}

int ffs_next(struct ffs *v, uint32_t dir, uint32_t *pos, struct ffs_ent *o)
{
    if (v->x) return xfs_next(v, dir, pos, o);
    uint32_t idx = *pos, nl = 0;
    int chain = 0, fits = 0;                                  /* a long name is being put together; it fits */
    uint8_t lsum = 0, lseq = 0;
    for (;; idx++) {
        uint8_t *e;
        uint32_t sec;
        int r = dir_ent(v, dir, idx, &e, &sec);
        if (r) { *pos = idx; return r; }
        if (e[0] == 0) { *pos = idx; return FFS_END; }
        if (e[0] == 0xe5) { chain = 0; continue; }
        if ((e[11] & 0x3f) == 0x0f) {                         /* a long-name piece */
            uint8_t seq = e[0] & 0x1f;
            if (e[0] & 0x40) {
                chain = seq >= 1 && seq <= 20;
                if (!chain) continue;
                fits = 1; nl = seq; lsum = e[13];
                zero(o->name, FFS_NAME);
            } else if (!chain || seq != lseq - 1 || e[13] != lsum) { chain = 0; continue; }
            lseq = seq;
            for (unsigned k = 0; k < 13 && fits; k++) {
                uint32_t ch = le16(e + lfn_at[k]), p = (seq - 1u) * 13 + k;
                if (ch == 0 || ch == 0xffff) break;
                if (p >= FFS_NAME - 1) { fits = 0; break; }       /* too long for the Amiga: the 8.3 name is used */
                o->name[p] = (char)(ch < 0x100 ? ch : '?');
            }
            continue;
        }
        if (e[11] & 0x08) { chain = 0; continue; }            /* the volume label */
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { chain = 0; continue; }
        int have = chain && lseq == 1 && lfn_sum(e) == lsum;
        fill_ent(v, o, e, dir, idx, sec);
        o->nlfn = have ? nl : 0;
        if (!have || !fits || !o->name[0]) sfn_text(e, o->name, 1);
        *pos = idx + 1;
        return 0;
    }
}

int ffs_find(struct ffs *v, uint32_t dir, const char *name, uint32_t len, struct ffs_ent *e)
{
    if (v->x) return xfs_find(v, dir, name, len, e);
    uint32_t pos = 0;
    for (;;) {
        int r = ffs_next(v, dir, &pos, e);
        if (r) return r;
        if (ffs_name_eq(e->name, name, len) || ffs_name_eq(e->sfn, name, len)) return 0;
    }
}

int ffs_parent(struct ffs *v, uint32_t dir, uint32_t *parent)
{
    if (v->x) return xfs_parent(v, dir, parent);
    if (!dir) { *parent = 0; return FFS_END; }
    uint8_t *e;
    int r = dir_ent(v, dir, 1, &e, 0);
    if (r) return r == FFS_END ? FFS_CORRUPT : r;
    if (e[0] != '.' || e[1] != '.' || !(e[11] & FFS_A_DIR)) return FFS_CORRUPT;
    uint32_t p = ent_first(v->f, e);
    if (v->f->fat32 && p == v->f->root_clus) p = 0;
    *parent = p;
    return 0;
}

int ffs_dir_ent(struct ffs *v, uint32_t dir, struct ffs_ent *e)
{
    if (v->x) return xfs_dir_ent(v, dir, e);
    uint32_t p, pos = 0;
    int r = ffs_parent(v, dir, &p);
    if (r) return r;
    for (;;) {
        r = ffs_next(v, p, &pos, e);
        if (r) return r == FFS_END ? FFS_CORRUPT : r;
        if ((e->attr & FFS_A_DIR) && e->first == dir) return 0;
    }
}

int ffs_resolve(struct ffs *v, const char *path, uint32_t *dir, struct ffs_ent *e)
{
    if (v->x) return xfs_resolve(v, path, dir, e);
    uint32_t d = 0;
    int any = 0;
    while (*path) {
        while (*path == '/') path++;
        uint32_t n = 0;
        while (path[n] && path[n] != '/') n++;
        if (!n) break;
        int r = ffs_find(v, d, path, n, e);
        if (r) return r;
        if (!(e->attr & FFS_A_DIR)) return FFS_NOTDIR;
        d = e->first;
        if (v->f->fat32 && d == v->f->root_clus) d = 0;
        path += n;
        any = 1;
    }
    if (!any) return FFS_BADNAME;                              /* not the partition's root */
    *dir = d;
    return 0;
}

/* ---- making entries --------------------------------------------------------------- */
int ffs_name_ok(const char *name, uint32_t len)
{
    if (!len || len > FFS_NAME - 1) return 0;
    if (name[len - 1] == '.' || name[len - 1] == ' ') return 0;   /* Windows drops these */
    if (len <= 2 && name[0] == '.' && (len == 1 || name[1] == '.')) return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
    }
    return 1;
}

static int sfn_char(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' || c == '@' || c == '~' || c == '`' ||
           c == '!' || c == '(' || c == ')' || c == '{' || c == '}' || c == '^' || c == '#' || c == '&';
}

/* a name that is a valid 8.3 name as it stands (each part in one case):
   1 and the entry's 11 bytes and case flags */
static int plain_83(const char *name, uint32_t len, uint8_t sfn[11], uint8_t *flags)
{
    uint32_t dot = len;
    for (uint32_t i = 0; i < len; i++) if (name[i] == '.') { if (dot != len) return 0; dot = i; }
    if (!dot || dot > 8 || (dot < len && (len - dot - 1 == 0 || len - dot - 1 > 3))) return 0;
    for (int i = 0; i < 11; i++) sfn[i] = ' ';
    *flags = 0;
    for (int part = 0; part < 2; part++) {
        uint32_t a = part ? dot + 1 : 0, b = part ? len : dot;
        int lo = 0, up = 0;
        for (uint32_t i = a; i < b; i++) {
            uint8_t c = (uint8_t)name[i];
            if (c >= 'a' && c <= 'z') lo = 1;
            else if (c >= 'A' && c <= 'Z') up = 1;
            uint8_t u = upper(c);
            if (!sfn_char(u)) return 0;
            sfn[(part ? 8 : 0) + (i - a)] = u;
        }
        if (lo && up) return 0;
        if (lo) *flags |= part ? 0x10 : 0x08;
    }
    return 1;
}

/* the basis of a generated 8.3 name ("LONGNA", "TXT") */
static void basis(const char *name, uint32_t len, uint8_t *base, uint32_t *blen, uint8_t ext[3])
{
    uint32_t dot = len;
    for (uint32_t i = len; i-- > 0;) if (name[i] == '.') { dot = i; break; }
    if (dot == 0) dot = len;                                  /* ".profile": no extension */
    uint32_t k = 0;
    for (uint32_t i = 0; i < dot && k < 8; i++) {
        uint8_t c = upper((uint8_t)name[i]);
        if (c == ' ' || c == '.') continue;
        base[k++] = sfn_char(c) ? c : '_';
    }
    if (!k) base[k++] = '_';
    *blen = k;
    ext[0] = ext[1] = ext[2] = ' ';
    for (uint32_t i = dot + 1, j = 0; i < len && j < 3; i++) {
        uint8_t c = upper((uint8_t)name[i]);
        if (c == ' ' || c == '.') continue;
        ext[j++] = sfn_char(c) ? c : '_';
    }
}

/* the ~N numbers already used in dir for this basis, and if sfn is taken */
static int sfn_taken(struct ffs *v, uint32_t dir, const uint8_t sfn[11])
{
    for (uint32_t idx = 0;; idx++) {
        uint8_t *e;
        int r = dir_ent(v, dir, idx, &e, 0);
        if (r == FFS_END) return 0;
        if (r) return r;
        if (e[0] == 0) return 0;
        if (e[0] == 0xe5 || (e[11] & 0x3f) == 0x0f) continue;
        int k = 0;
        while (k < 11 && e[k] == sfn[k]) k++;
        if (k == 11) return 1;
    }
}

static int make_sfn(struct ffs *v, uint32_t dir, const char *name, uint32_t len, uint8_t sfn[11])
{
    uint8_t base[8], ext[3];
    uint32_t blen;
    basis(name, len, base, &blen, ext);
    for (uint32_t n = 1; n < 1000000; n++) {
        char num[8];
        uint32_t d = 0, x = n;
        do { num[d++] = (char)('0' + x % 10); x /= 10; } while (x);
        uint32_t keep = blen < 7 - d ? blen : 7 - d, k = 0;
        for (uint32_t i = 0; i < keep; i++) sfn[k++] = base[i];
        sfn[k++] = '~';
        while (d) sfn[k++] = (uint8_t)num[--d];
        while (k < 8) sfn[k++] = ' ';
        sfn[8] = ext[0]; sfn[9] = ext[1]; sfn[10] = ext[2];
        int t = sfn_taken(v, dir, sfn);
        if (t < 0) return t;
        if (!t) return 0;
    }
    return FFS_EXISTS;
}

/* need free entries in a row in dir; the index of the first. Grows the directory */
static int find_slots(struct ffs *v, uint32_t dir, uint32_t need, uint32_t *at)
{
    uint32_t run = 0, start = 0, idx = 0;
    for (;; idx++) {
        uint8_t *e;
        int r = dir_ent(v, dir, idx, &e, 0);
        if (r == FFS_END) break;
        if (r) return r;
        if (e[0] == 0 || e[0] == 0xe5) {
            if (!run) start = idx;
            if (++run == need) { *at = start; return 0; }
        } else run = 0;
    }
    if (fixed_root(v, dir)) return FFS_DIRFULL;
    if (idx >= 65536) return FFS_DIRFULL;                      /* FAT's limit for a directory */
    /* one more cluster, zeroed (end-of-directory marks) */
    uint32_t last = v->dc_clus, c;                            /* dir_sec left the walk at its last cluster */
    int r = alloc_clus(v, last + 1, &c);
    if (r) return r;
    zero(v->bounce, HDF_BLOCK);
    for (uint32_t s = 0; s < v->f->spc; s++) if (wr(v, clus_sec(v->f, c) + s, 1, v->bounce)) return v->err;
    if ((r = fat_set(v, last, c))) return r;
    if (!run) start = idx;
    *at = start;
    return 0;
}

static void stamp(struct ffs *v, uint8_t *e, int created)
{
    put16(e + 22, v->time); put16(e + 24, v->date);
    put16(e + 18, v->date);                                   /* last access */
    if (created) { e[13] = 0; put16(e + 14, v->time); put16(e + 16, v->date); }
}

/* the entries for a name at dir: long-name pieces, then the 8.3 entry from
   tmpl (its bytes 11..31); the result in o */
static int put_name(struct ffs *v, uint32_t dir, const char *name, uint32_t len, const uint8_t *tmpl, struct ffs_ent *o)
{
    uint8_t sfn[11], flags = 0;
    int lfn = !plain_83(name, len, sfn, &flags);
    if (!lfn) {
        int t = sfn_taken(v, dir, sfn);
        if (t < 0) return t;
        if (t) lfn = 1;                                       /* the 8.3 name is someone else's: make one */
    }
    if (lfn) { int r = make_sfn(v, dir, name, len, sfn); if (r) return r; flags = 0; }
    uint32_t pieces = lfn ? (len + 12) / 13 : 0, at = 0;
    int r = find_slots(v, dir, pieces + 1, &at);
    if (r) return r;
    uint8_t sum = lfn_sum(sfn);
    for (uint32_t p = 0; p < pieces; p++) {
        uint32_t seq = pieces - p;                             /* the last piece comes first */
        uint8_t *e;
        if ((r = dir_ent(v, dir, at + p, &e, 0))) return r == FFS_END ? FFS_CORRUPT : r;
        zero(e, 32);
        e[0] = (uint8_t)(seq | (p == 0 ? 0x40 : 0));
        e[11] = 0x0f;
        e[13] = sum;
        for (unsigned k = 0; k < 13; k++) {
            uint32_t i = (seq - 1) * 13 + k, ch = i < len ? (uint8_t)name[i] : i == len ? 0 : 0xffff;
            put16(e + lfn_at[k], ch);
        }
        v->dsec_dirty = 1;
    }
    uint8_t *e;
    uint32_t sec;
    if ((r = dir_ent(v, dir, at + pieces, &e, &sec))) return r == FFS_END ? FFS_CORRUPT : r;
    copy(e + 11, tmpl + 11, 21);
    copy(e, sfn, 11);
    if (e[0] == 0xe5) e[0] = 0x05;
    e[12] = flags;
    v->dsec_dirty = 1;
    fill_ent(v, o, e, dir, at + pieces, sec);
    o->nlfn = pieces;
    uint32_t k = 0;
    for (; k < len; k++) o->name[k] = name[k];
    o->name[k] = 0;
    return 0;
}

int ffs_create(struct ffs *v, uint32_t dir, const char *name, uint32_t len, int isdir, struct ffs_ent *o)
{
    if (v->x) return xfs_create(v, dir, name, len, isdir, o);
    if (!ffs_name_ok(name, len)) return FFS_BADNAME;
    struct ffs_ent x;
    int r = ffs_find(v, dir, name, len, &x);
    if (r == 0) return FFS_EXISTS;
    if (r != FFS_END) return r;
    uint8_t t[32];
    zero(t, 32);
    stamp(v, t, 1);
    t[11] = isdir ? FFS_A_DIR : FFS_A_ARCH;
    uint32_t c = 0;
    if (isdir) {                                              /* its cluster, with "." and ".." */
        if ((r = alloc_clus(v, 0, &c))) return r;
        for (uint32_t s = 0; s < v->f->spc; s++) {
            zero(v->bounce, HDF_BLOCK);
            if (!s) {
                for (int i = 0; i < 2; i++) {
                    uint8_t *e = v->bounce + 32 * i;
                    for (int k = 0; k < 11; k++) e[k] = ' ';
                    e[0] = '.'; if (i) e[1] = '.';
                    e[11] = FFS_A_DIR;
                    stamp(v, e, 1);
                    set_first(v->f, e, i ? dir : c);
                }
            }
            if (wr(v, clus_sec(v->f, c) + s, 1, v->bounce)) { free_chain(v, c); return v->err; }
        }
        set_first(v->f, t, c);
    }
    r = put_name(v, dir, name, len, t, o);
    if (r && c) free_chain(v, c);
    return r;
}

static int kill_entries(struct ffs *v, const struct ffs_ent *e)
{
    for (uint32_t i = e->idx - e->nlfn; i <= e->idx; i++) {
        uint8_t *p;
        int r = dir_ent(v, e->dir, i, &p, 0);
        if (r) return r == FFS_END ? FFS_CORRUPT : r;
        p[0] = 0xe5;
        v->dsec_dirty = 1;
    }
    return 0;
}

int ffs_delete(struct ffs *v, const struct ffs_ent *e)
{
    if (v->x) return xfs_delete(v, e);
    if (e->attr & FFS_A_DIR) {
        struct ffs_ent x;
        uint32_t pos = 0;
        int r = ffs_next(v, e->first, &pos, &x);
        if (r == 0) return FFS_NOTEMPTY;
        if (r != FFS_END) return r;
        if (v->dc_dir == e->first) v->dc_dir = ~0u;
    }
    int r = kill_entries(v, e);
    if (r) return r;
    return e->first ? free_chain(v, e->first) : 0;
}

int ffs_rename(struct ffs *v, const struct ffs_ent *e, uint32_t ndir, const char *name, uint32_t len, struct ffs_ent *o)
{
    if (v->x) return xfs_rename(v, e, ndir, name, len, o);
    if (!ffs_name_ok(name, len)) return FFS_BADNAME;
    struct ffs_ent x;
    int r = ffs_find(v, ndir, name, len, &x);
    if (r == 0 && !(x.dir == e->dir && x.idx == e->idx)) return FFS_EXISTS;   /* another one: a case change is fine */
    if (r && r != FFS_END) return r;
    int isdir = (e->attr & FFS_A_DIR) != 0;
    if (isdir && ndir != e->dir) {                            /* not into itself */
        uint32_t d = ndir, guard = 4096;
        while (d && guard--) {
            if (d == e->first) return FFS_LOOP;
            if ((r = ffs_parent(v, d, &d))) return r;
        }
    }
    uint8_t t[32];
    uint8_t *p;
    if ((r = dir_ent(v, e->dir, e->idx, &p, 0))) return r;
    copy(t, p, 32);
    if ((r = put_name(v, ndir, name, len, t, o))) return r;
    /* put_name may have written where e's entries were not: they are still there */
    if ((r = kill_entries(v, e))) return r;
    if (isdir && ndir != e->dir) {                            /* its ".." */
        if ((r = dir_ent(v, e->first, 1, &p, 0))) return r == FFS_END ? FFS_CORRUPT : r;
        set_first(v->f, p, ndir);
        v->dsec_dirty = 1;
    }
    return 0;
}

static int edit_ent(struct ffs *v, struct ffs_ent *e, uint8_t **p)
{
    *p = dsec_load(v, e->sec);
    if (!*p) return v->err;
    *p += 32 * e->slot;
    v->dsec_dirty = 1;
    return 0;
}

int ffs_set_attr(struct ffs *v, struct ffs_ent *e, uint8_t attr)
{
    if (v->x) return xfs_set_attr(v, e, attr);
    uint8_t *p;
    int r = edit_ent(v, e, &p);
    if (r) return r;
    p[11] = (uint8_t)((p[11] & (FFS_A_DIR | 0x08)) | (attr & (FFS_A_RO | FFS_A_HIDDEN | FFS_A_SYSTEM | FFS_A_ARCH)));
    e->attr = p[11];
    return 0;
}

int ffs_set_date(struct ffs *v, struct ffs_ent *e, uint16_t date, uint16_t time)
{
    if (v->x) return xfs_set_date(v, e, date, time);
    uint8_t *p;
    int r = edit_ent(v, e, &p);
    if (r) return r;
    put16(p + 22, time); put16(p + 24, date);
    e->date = date; e->time = time;
    return 0;
}

/* ---- files ---------------------------------------------------------------------------- */
void ffs_open(struct ffs_file *h, const struct ffs_ent *e)
{
    h->e = *e;
    h->ci = ~0u;
    h->cc = 0;
    h->dirty = 0;
}

int ffs_flush_file(struct ffs *v, struct ffs_file *h)
{
    if (v->x) return xfs_flush_file(v, h);
    if (!h->dirty) return 0;
    uint8_t *p;
    int r = edit_ent(v, &h->e, &p);
    if (r) return r;
    set_first(v->f, p, h->e.first);
    put32(p + 28, (h->e.attr & FFS_A_DIR) ? 0 : h->e.size);
    put16(p + 22, h->e.time); put16(p + 24, h->e.date); put16(p + 18, h->e.date);
    p[11] |= FFS_A_ARCH;
    h->e.attr = p[11];
    h->dirty = 0;
    return 0;
}

/* the file's ci-th cluster; with grow, clusters are added up to it */
static int file_clus(struct ffs *v, struct ffs_file *h, uint32_t ci, int grow, uint32_t *out)
{
    uint32_t k, c;
    if (!h->e.first) {
        if (!grow) return FFS_END;
        int r = alloc_clus(v, 0, &c);
        if (r) return r;
        h->e.first = c;
        h->dirty = 1;
        h->ci = 0; h->cc = c;
    }
    if (h->ci != ~0u && h->ci <= ci) { k = h->ci; c = h->cc; }
    else { k = 0; c = h->e.first; }
    while (k < ci) {
        uint32_t n = chain_next(v, c);
        if (n == ~0u) return v->err ? v->err : FFS_CORRUPT;
        if (!n) {
            if (!grow) return FFS_END;
            int r = alloc_clus(v, c + 1, &n);
            if (r) { h->ci = k; h->cc = c; return r; }
            if ((r = fat_set(v, c, n))) return r;
        }
        c = n; k++;
    }
    h->ci = k; h->cc = c;
    *out = c;
    return 0;
}

/* sectors at pos that lie one after another on the card (up to max) */
static int run_at(struct ffs *v, struct ffs_file *h, uint32_t pos, uint32_t max, int grow, uint32_t *sec, uint32_t *n)
{
    uint32_t cb = ffs_cluster_bytes(v), ci = pos / cb, c;
    int r = file_clus(v, h, ci, grow, &c);
    if (r) return r;
    uint32_t off = (pos % cb) / HDF_BLOCK;
    *sec = clus_sec(v->f, c) + off;
    uint32_t k = v->f->spc - off;
    /* following clusters next to this one on the card, while they are needed */
    while (k < max) {
        uint32_t nc;
        uint32_t save_ci = h->ci, save_cc = h->cc;
        r = file_clus(v, h, ci + 1, grow, &nc);
        if (r || nc != c + 1) { h->ci = save_ci; h->cc = save_cc; break; }
        c = nc; ci++;
        k += v->f->spc;
    }
    *n = k < max ? k : max;
    return 0;
}

static int aligned(const void *p) { return !((uintptr_t)p & 3); }

int32_t ffs_read(struct ffs *v, struct ffs_file *h, uint32_t pos, void *buf, uint32_t n)
{
    if (v->x) return xfs_read(v, h, pos, buf, n);
    uint8_t *p = buf;
    if (pos >= h->e.size) return 0;
    if (n > h->e.size - pos) n = h->e.size - pos;
    uint32_t done = 0;
    while (done < n) {
        uint32_t off = pos % HDF_BLOCK, left = n - done, sec, cnt;
        int whole = !off && left >= HDF_BLOCK && aligned(p);
        int r = run_at(v, h, pos, whole ? left / HDF_BLOCK : 1, 0, &sec, &cnt);
        if (r) return done ? (int32_t)done : (r == FFS_END ? FFS_CORRUPT : r);
        if (whole) {
            if (rd(v, sec, cnt, p)) return done ? (int32_t)done : v->err;
            cnt *= HDF_BLOCK;
        } else {
            if (rd(v, sec, 1, v->bounce)) return done ? (int32_t)done : v->err;
            cnt = HDF_BLOCK - off < left ? HDF_BLOCK - off : left;
            copy(p, v->bounce + off, cnt);
        }
        p += cnt; pos += cnt; done += cnt;
    }
    return (int32_t)done;
}

int32_t ffs_write(struct ffs *v, struct ffs_file *h, uint32_t pos, const void *buf, uint32_t n, int *err)
{
    if (v->x) return xfs_write(v, h, pos, buf, n, err);
    const uint8_t *p = buf;
    uint32_t done = 0;
    *err = 0;
    if (pos > h->e.size) { *err = FFS_CORRUPT; return 0; }
    if (n > 0xffffffffu - pos) n = 0xffffffffu - pos;          /* FAT's 4 GB */
    while (done < n) {
        uint32_t off = pos % HDF_BLOCK, left = n - done, sec, cnt;
        int whole = !off && left >= HDF_BLOCK && aligned(p);
        int r = run_at(v, h, pos, whole ? left / HDF_BLOCK : 1, 1, &sec, &cnt);
        if (r) { *err = r; break; }
        if (whole) {
            if (wr(v, sec, cnt, p)) { *err = v->err; break; }
            cnt *= HDF_BLOCK;
        } else {
            cnt = HDF_BLOCK - off < left ? HDF_BLOCK - off : left;
            if (pos - off < h->e.size) {                      /* the sector holds data: read it first */
                if (rd(v, sec, 1, v->bounce)) { *err = v->err; break; }
            } else zero(v->bounce, HDF_BLOCK);
            copy(v->bounce + off, p, cnt);
            if (wr(v, sec, 1, v->bounce)) { *err = v->err; break; }
        }
        p += cnt; pos += cnt; done += cnt;
        if (pos > h->e.size) h->e.size = pos;
    }
    if (done) { h->e.date = v->date; h->e.time = v->time; h->dirty = 1; }
    int r = ffs_flush_file(v, h);
    if (r && !*err) *err = r;
    return (int32_t)done;
}

int ffs_truncate(struct ffs *v, struct ffs_file *h, uint32_t size)
{
    if (v->x) return xfs_truncate(v, h, size);
    int r;
    if (size > h->e.size) {                                   /* longer: zeros */
        uint32_t pos = h->e.size;
        while (pos < size) {
            uint32_t off = pos % HDF_BLOCK, sec, cnt;
            if ((r = run_at(v, h, pos, 1, 1, &sec, &cnt))) { ffs_flush_file(v, h); return r; }
            if (off) { if (rd(v, sec, 1, v->bounce)) return v->err; }
            else zero(v->bounce, HDF_BLOCK);
            zero(v->bounce + off, HDF_BLOCK - off);
            if (wr(v, sec, 1, v->bounce)) return v->err;
            pos += HDF_BLOCK - off;
            h->e.size = pos < size ? pos : size;
            h->dirty = 1;
        }
    } else if (size < h->e.size) {
        uint32_t cb = ffs_cluster_bytes(v), keep = (size + cb - 1) / cb;
        if (!keep) {
            if (h->e.first && (r = free_chain(v, h->e.first))) return r;
            h->e.first = 0;
        } else {
            uint32_t c;
            if ((r = file_clus(v, h, keep - 1, 0, &c))) return r == FFS_END ? FFS_CORRUPT : r;
            uint32_t n = chain_next(v, c);
            if (n == ~0u) return v->err ? v->err : FFS_CORRUPT;
            if ((r = fat_set(v, c, eoc(v)))) return r;
            if (n && (r = free_chain(v, n))) return r;
        }
        h->ci = ~0u;
        h->e.size = size;
        h->dirty = 1;
    }
    if (h->dirty) { h->e.date = v->date; h->e.time = v->time; }
    return ffs_flush_file(v, h);
}

/* ---- dates ------------------------------------------------------------------------------- */
static int leap(uint32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void ffs_amiga_to_fat(uint32_t days, uint32_t mins, uint32_t ticks, uint16_t *date, uint16_t *time)
{
    if (days < 730) { *date = (1 << 5) | 1; *time = 0; return; }   /* before 1980: FAT's first day */
    uint32_t d = days - 730, y = 1980;
    while (d >= (uint32_t)(leap(y) ? 366 : 365)) { d -= leap(y) ? 366 : 365; y++; }
    uint32_t m = 0;
    for (; m < 11; m++) {
        uint32_t ml = mdays[m] + (m == 1 && leap(y));
        if (d < ml) break;
        d -= ml;
    }
    if (y > 2107) y = 2107;
    *date = (uint16_t)(((y - 1980) << 9) | ((m + 1) << 5) | (d + 1));
    uint32_t s = ticks / 50;
    *time = (uint16_t)(((mins / 60) << 11) | ((mins % 60) << 5) | (s / 2));
}

void ffs_fat_to_amiga(uint16_t date, uint16_t time, uint32_t *days, uint32_t *mins, uint32_t *ticks)
{
    uint32_t y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12) m = 1;
    if (d < 1) d = 1;
    uint32_t n = 730;
    for (uint32_t k = 1980; k < y; k++) n += leap(k) ? 366 : 365;
    for (uint32_t k = 0; k + 1 < m; k++) n += mdays[k] + (k == 1 && leap(y));
    *days = n + d - 1;
    uint32_t h = time >> 11, mi = (time >> 5) & 63, s = (time & 31) * 2;
    if (h > 23) h = 23;
    if (mi > 59) mi = 59;
    if (s > 59) s = 59;
    *mins = h * 60 + mi;
    *ticks = s * 50;
}
