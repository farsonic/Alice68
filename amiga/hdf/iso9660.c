/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 CD images: ISO 9660 (ECMA-119) with Joliet and Rock Ridge (SUSP/RRIP)
 * names, read-only. See iso9660.h. Written from the ECMA-119 standard, the
 * Joliet specification and IEEE P1282 (Rock Ridge); no code copied.
 */
#include "iso9660.h"

static void zero(void *p, uint32_t n) { uint8_t *q = p; while (n--) *q++ = 0; }
static void copy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }

#define SPB (ISO_SEC / HDF_BLOCK)       /* card blocks per ISO sector */

/* one sector of the image */
static int rd_sec(struct iso *v, uint32_t sec, uint8_t *buf)
{
    if (sec >= v->f->blocks / SPB) return ISO_CORRUPT;
    return hdf_read(v->io, v->f, sec * SPB, SPB, buf) ? ISO_IO : ISO_OK;
}
static int dir_sec(struct iso *v, uint32_t sec)
{
    if (v->buf_sec == sec) return ISO_OK;
    v->buf_sec = ~0u;
    int r = rd_sec(v, sec, v->buf);
    if (!r) v->buf_sec = sec;
    return r;
}

/* ---- dates ------------------------------------------------------------------- */
static int leap(uint32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void iso_date(const uint8_t d[7], uint32_t *days, uint32_t *mins, uint32_t *ticks)
{
    uint32_t y = 1900u + d[0], m = d[1], dd = d[2];
    *days = *mins = *ticks = 0;
    if (y < 1978) return;                                   /* before AmigaDOS's epoch, or no date */
    if (m < 1 || m > 12) m = 1;
    if (dd < 1 || dd > 31) dd = 1;
    uint32_t n = 0;
    for (uint32_t k = 1978; k < y; k++) n += leap(k) ? 366 : 365;
    for (uint32_t k = 0; k + 1 < m; k++) n += mdays[k] + (k == 1 && leap(y));
    *days = n + dd - 1;
    uint32_t h = d[3] > 23 ? 23 : d[3], mi = d[4] > 59 ? 59 : d[4], s = d[5] > 59 ? 59 : d[5];
    *mins = h * 60 + mi;
    *ticks = s * 50;
}

/* ---- names ------------------------------------------------------------------- */
static uint8_t lower(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) return (uint8_t)(c + 32);
    return c;
}
int iso_name_eq(const char *a, const char *b, uint32_t blen)
{
    uint32_t i = 0;
    for (; i < blen && a[i]; i++) if (lower((uint8_t)a[i]) != lower((uint8_t)b[i])) return 0;
    return i == blen && !a[i];
}

/* AmigaDOS cannot have ':' or '/' in a name */
static void tidy(char *n)
{
    for (char *p = n; *p; p++) if (*p == ':' || *p == '/') *p = '_';
    if (!n[0]) { n[0] = '_'; n[1] = 0; }
}

/* ISO 9660's own name: "NAME.EXT;1" -> "NAME.EXT", "NAME.;1" -> "NAME" */
static void plain_name(const uint8_t *s, uint32_t len, int ucs2, char *out)
{
    uint32_t n = 0;
    if (ucs2) {
        for (uint32_t i = 0; i + 1 < len && n < ISO_NAME - 1; i += 2) {
            uint32_t c = (uint32_t)s[i] << 8 | s[i + 1];
            out[n++] = (char)(c < 0x100 ? c : '?');
        }
    } else {
        for (uint32_t i = 0; i < len && n < ISO_NAME - 1; i++) out[n++] = (char)s[i];
    }
    out[n] = 0;
    for (uint32_t i = 0; i < n; i++) if (out[i] == ';') { out[i] = 0; n = i; break; }
    if (n > 1 && out[n - 1] == '.') out[--n] = 0;
}

/* the Rock Ridge entries of a record's System Use area (and of its continuation
   areas): the NM name, RE (a relocated directory: hidden here), CL (where a
   relocated directory is), PL (a relocated directory's real parent). 1: a name */
struct rr_info { int skip; uint32_t cl, pl; };
static int rr_entries(struct iso *v, const uint8_t *rec, uint32_t su, uint32_t len, char *out, struct rr_info *ri)
{
    const uint8_t *a = rec;
    uint32_t p = su, end = len, n = 0;
    int have = 0;
    ri->skip = 0; ri->cl = ri->pl = 0;
    for (int area = 0; area < 8; area++) {
        uint32_t ce_blk = 0, ce_off = 0, ce_len = 0;
        int ce = 0;
        while (p + 4 <= end) {
            const uint8_t *s = a + p;
            uint32_t l = s[2];
            if (l < 4 || p + l > end) break;
            if (s[0] == 'N' && s[1] == 'M' && l >= 5) {
                if (!(s[4] & 6)) {                          /* not "." or ".." */
                    for (uint32_t k = 5; k < l && n < ISO_NAME - 1; k++) out[n++] = (char)s[k];
                    have = 1;
                }
            } else if (s[0] == 'R' && s[1] == 'E') ri->skip = 1;
            else if (s[0] == 'C' && s[1] == 'L' && l >= 12) ri->cl = le32(s + 4);
            else if (s[0] == 'P' && s[1] == 'L' && l >= 12) ri->pl = le32(s + 4);
            else if (s[0] == 'C' && s[1] == 'E' && l >= 28) { ce_blk = le32(s + 4); ce_off = le32(s + 12); ce_len = le32(s + 20); ce = 1; }
            else if (s[0] == 'S' && s[1] == 'T') break;     /* the terminator */
            p += l;
        }
        if (!ce || ce_off >= ISO_SEC || ce_len > ISO_SEC - ce_off || rd_sec(v, ce_blk, v->ce)) break;
        a = v->ce; p = ce_off; end = ce_off + ce_len;
    }
    out[n] = 0;
    /* names from today's systems are UTF-8: to Latin-1 when they are valid UTF-8 (else as they are) */
    uint32_t i = 0, k = 0;
    int utf8 = 1, multi = 0;
    while (i < n) {
        uint8_t c = (uint8_t)out[i];
        uint32_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!len || i + len > n) { utf8 = 0; break; }
        for (uint32_t j = 1; j < len; j++) if (((uint8_t)out[i + j] & 0xc0) != 0x80) utf8 = 0;
        if (!utf8) break;
        if (len > 1) multi = 1;
        i += len;
    }
    if (utf8 && multi) {
        for (i = 0; i < n; ) {
            uint8_t c = (uint8_t)out[i];
            if (c < 0x80) { out[k++] = (char)c; i++; continue; }
            uint32_t len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
            uint32_t cp = len == 2 ? (c & 0x1fu) : len == 3 ? (c & 0x0fu) : (c & 0x07u);
            for (uint32_t j = 1; j < len; j++) cp = cp << 6 | ((uint8_t)out[i + j] & 0x3fu);
            out[k++] = (char)(cp < 0x100 ? cp : '?');
            i += len;
        }
        out[k] = 0;
        n = k;
    }
    return have && n;
}

/* one directory record (len bytes): 1 = not shown (".", "..", relocated), 0 = e filled, <0 broken */
static int parse_rec(struct iso *v, const uint8_t *r, uint32_t len, struct iso_ent *e, uint32_t *cl)
{
    uint32_t nl = r[32];
    *cl = 0;
    if (33 + nl > len) return ISO_CORRUPT;
    zero(e, sizeof *e);
    e->extent = le32(r + 2) + r[1];                         /* after the extended attribute record, if any */
    e->size = le32(r + 10);
    copy(e->date, r + 18, 7);
    e->isdir = (r[25] & 2) != 0;
    e->hidden = (r[25] & 1) != 0;
    if (nl == 1 && (r[33] == 0 || r[33] == 1)) return 1;
    int have = 0;
    if (v->rr) {
        struct rr_info ri = { 0, 0, 0 };
        uint32_t su = 33 + nl + ((nl & 1) ? 0 : 1) + v->rr_skip;
        if (su < len) have = rr_entries(v, r, su, len, e->name, &ri);
        if (ri.skip) return 1;
        *cl = ri.cl;
    }
    if (!have) plain_name(r + 33, nl, v->joliet && !v->rr, e->name);
    tidy(e->name);
    return 0;
}

/* ---- the volume ----------------------------------------------------------------- */
static void label(const uint8_t *s, uint32_t len, int ucs2, char *out)
{
    plain_name(s, len, ucs2, out);
    uint32_t n = 0;
    while (out[n]) n++;
    while (n && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
    if (n > 30) out[n = 30] = 0;
    for (char *p = out; *p; p++) if (*p == ':' || *p == '/') *p = '_';
}

int iso_open(struct iso *v, struct hdf_io *io, const struct hdf_file *f)
{
    zero(v, sizeof *v);
    v->io = io; v->f = f; v->buf_sec = ~0u;
    v->buf = io->alloc(io->ctx, 2 * ISO_SEC + HDF_BLOCK);
    if (!v->buf) return ISO_IO;
    v->ce = v->buf + ISO_SEC;
    v->bounce = v->ce + ISO_SEC;
    v->blocks = f->blocks / SPB;
    uint8_t proot[34], jroot[34];
    char jvol[33];
    uint8_t vdate[7];
    int have_p = 0, have_j = 0, have_vdate = 0, r = ISO_NOTISO;
    jvol[0] = 0;
    for (uint32_t s = 16; s < 16 + 32 && s < v->blocks; s++) {
        if ((r = rd_sec(v, s, v->buf))) goto fail;
        const uint8_t *b = v->buf;
        if (b[1] != 'C' || b[2] != 'D' || b[3] != '0' || b[4] != '0' || b[5] != '1') break;
        if (b[0] == 255) break;
        if (b[0] == 1 && !have_p) {
            if (le16(b + 128) != ISO_SEC) { r = ISO_NOTISO; goto fail; }   /* other sector sizes: not on CDs */
            have_p = 1;
            copy(proot, b + 156, 34);
            label(b + 40, 32, 0, v->vol);
            /* the volume's creation date, "YYYYMMDDHHMMSScc": the volume's and the root's date */
            const uint8_t *c = b + 813;
            uint32_t num[6], ok = 1;
            static const uint8_t w[6] = { 4, 2, 2, 2, 2, 2 };
            for (uint32_t k = 0, at = 0; k < 6; at += w[k], k++) {
                num[k] = 0;
                for (uint32_t j = 0; j < w[k]; j++) {
                    uint8_t ch = c[at + j];
                    if (ch < '0' || ch > '9') ok = 0;
                    num[k] = num[k] * 10 + (uint32_t)(ch - '0');
                }
            }
            if (ok && num[0] >= 1978 && num[0] < 2156) {
                vdate[0] = (uint8_t)(num[0] - 1900); vdate[1] = (uint8_t)num[1]; vdate[2] = (uint8_t)num[2];
                vdate[3] = (uint8_t)num[3]; vdate[4] = (uint8_t)num[4]; vdate[5] = (uint8_t)num[5]; vdate[6] = 0;
                have_vdate = 1;
            }
            uint32_t space = le32(b + 80);
            if (space && space < v->blocks) v->blocks = space;
        } else if (b[0] == 2 && !have_j && b[88] == 0x25 && b[89] == 0x2f && (b[90] == 0x40 || b[90] == 0x43 || b[90] == 0x45)) {
            have_j = 1;                                     /* Joliet: escape sequence %/@, %/C or %/E */
            copy(jroot, b + 156, 34);
            label(b + 40, 32, 1, jvol);
        }
    }
    v->buf_sec = ~0u;
    if (!have_p) { r = ISO_NOTISO; goto fail; }
    if (jvol[0]) copy(v->vol, jvol, 33);                    /* Joliet's label keeps its case */

    /* Rock Ridge: the root's "." record starts its System Use area with SP */
    uint32_t cl;
    struct iso_ent t;
    if (parse_rec(v, proot, 34, &t, &cl) < 0) { r = ISO_CORRUPT; goto fail; }
    if ((r = dir_sec(v, t.extent))) goto fail;
    const uint8_t *dot = v->buf;
    if (dot[0] >= 34 + 7 && dot[32] == 1 && dot[33] == 0) {
        const uint8_t *sp = dot + 34;
        if (sp[0] == 'S' && sp[1] == 'P' && sp[4] == 0xbe && sp[5] == 0xef) { v->rr = 1; v->rr_skip = sp[6]; }
    }
    const uint8_t *root = proot;
    if (!v->rr && have_j) { v->joliet = 1; root = jroot; }
    v->root.extent = le32(root + 2) + root[1];
    v->root.size = le32(root + 10);
    copy(v->root.date, have_vdate ? vdate : root + 18, 7);
    v->root.isdir = 1;
    v->root.dir = v->root.extent;
    copy(v->root.name, v->vol, 33);
    if (v->root.extent >= v->blocks || !v->root.size) { r = ISO_CORRUPT; goto fail; }
    return ISO_OK;
fail:
    iso_close(v);
    return r;
}

void iso_close(struct iso *v)
{
    if (v->buf) v->io->free(v->io->ctx, v->buf, 2 * ISO_SEC + HDF_BLOCK);
    v->buf = v->ce = v->bounce = 0;
}

/* ---- directories ------------------------------------------------------------------- */
/* a directory's size, from its own "." record */
static int dir_size(struct iso *v, uint32_t ext, uint32_t *size)
{
    int r = dir_sec(v, ext);
    if (r) return r;
    const uint8_t *d = v->buf;
    if (d[0] < 34 || d[32] != 1 || d[33] != 0) return ISO_CORRUPT;
    *size = le32(d + 10);
    return ISO_OK;
}

int iso_next(struct iso *v, const struct iso_ent *dir, uint32_t *pos, struct iso_ent *e)
{
    if (!dir->isdir) return ISO_NOTDIR;
    for (uint32_t guard = 0; *pos < dir->size && guard < 1000000; guard++) {
        uint32_t off = *pos % ISO_SEC;
        int r = dir_sec(v, dir->extent + *pos / ISO_SEC);
        if (r) return r;
        const uint8_t *rec = v->buf + off;
        uint32_t len = rec[0];
        if (len < 34 || off + len > ISO_SEC) { *pos = (*pos / ISO_SEC + 1) * ISO_SEC; continue; }   /* the rest of the sector is padding */
        uint32_t at = *pos, cl;
        uint8_t flags = rec[25];
        *pos += len;
        int s = parse_rec(v, rec, len, e, &cl);
        if (s) continue;
        /* Rock Ridge: the folder mkisofs keeps relocated deep folders in (each is shown where it belongs) */
        if (v->rr && dir->extent == v->root.extent && e->isdir &&
            (iso_name_eq(e->name, "rr_moved", 8) || iso_name_eq(e->name, ".rr_moved", 9))) continue;
        e->dir = dir->extent;
        e->pos = at;
        /* a file in several extents: the following records, one after the other on the image */
        for (int g = 0; (flags & 0x80) && g < 64 && *pos < dir->size; g++) {
            uint32_t o2 = *pos % ISO_SEC;
            if ((r = dir_sec(v, dir->extent + *pos / ISO_SEC))) return r;
            const uint8_t *q = v->buf + o2;
            if (q[0] < 34 || o2 + q[0] > ISO_SEC) { *pos = (*pos / ISO_SEC + 1) * ISO_SEC; continue; }
            *pos += q[0];
            e->size += le32(q + 10);
            flags = q[25];
        }
        if (cl) {                                           /* Rock Ridge: a directory moved elsewhere (deep trees) */
            e->isdir = 1;
            e->extent = cl;
            if ((r = dir_size(v, cl, &e->size))) return r;
        }
        return ISO_OK;
    }
    return ISO_END;
}

int iso_find(struct iso *v, const struct iso_ent *dir, const char *name, uint32_t len, struct iso_ent *e)
{
    uint32_t pos = 0;
    int r;
    while ((r = iso_next(v, dir, &pos, e)) == ISO_OK)
        if (iso_name_eq(e->name, name, len)) return ISO_OK;
    return r;
}

/* the parent of directory ext, from its ".." record (Rock Ridge PL first) */
static int dotdot(struct iso *v, uint32_t ext, uint32_t *parent)
{
    int r = dir_sec(v, ext);
    if (r) return r;
    const uint8_t *d = v->buf;
    uint32_t l = d[0];
    if (l < 34 || d[32] != 1 || d[33] != 0 || l + 34 > ISO_SEC) return ISO_CORRUPT;
    const uint8_t *p = d + l;
    if (p[0] < 34 || l + p[0] > ISO_SEC || p[32] != 1 || p[33] != 1) return ISO_CORRUPT;
    *parent = le32(p + 2) + p[1];
    if (v->rr && p[0] > 34 + v->rr_skip) {
        struct rr_info ri;
        char tmp[ISO_NAME];
        uint8_t rec[256];
        copy(rec, p, p[0]);                                 /* rr_entries may read a continuation area */
        rr_entries(v, rec, 34 + v->rr_skip, rec[0], tmp, &ri);
        if (ri.pl) *parent = ri.pl;
    }
    return ISO_OK;
}

/* a directory's own entry, by its extent */
static int dir_ent(struct iso *v, uint32_t ext, struct iso_ent *out)
{
    if (ext == v->root.extent) { *out = v->root; return ISO_OK; }
    uint32_t p;
    int r = dotdot(v, ext, &p);
    if (r) return r;
    struct iso_ent pd;
    zero(&pd, sizeof pd);
    pd.isdir = 1;
    pd.extent = p;
    if (p == v->root.extent) pd = v->root;
    else if ((r = dir_size(v, p, &pd.size))) return r;
    uint32_t pos = 0;
    while ((r = iso_next(v, &pd, &pos, out)) == ISO_OK)
        if (out->isdir && out->extent == ext) return ISO_OK;
    return r == ISO_END ? ISO_CORRUPT : r;
}

int iso_parent(struct iso *v, const struct iso_ent *e, struct iso_ent *parent)
{
    if (e->isdir && e->extent == v->root.extent) { *parent = v->root; return ISO_OK; }
    return dir_ent(v, e->dir, parent);
}

/* ---- files ---------------------------------------------------------------------------- */
int32_t iso_read(struct iso *v, const struct iso_ent *e, uint32_t pos, void *buf, uint32_t n)
{
    if (e->isdir) return ISO_NOTDIR;
    if (pos >= e->size) return 0;
    if (n > e->size - pos) n = e->size - pos;
    uint8_t *p = buf;
    uint32_t done = 0, base = e->extent * SPB;
    while (done < n) {
        uint32_t blk = base + pos / HDF_BLOCK, off = pos % HDF_BLOCK, left = n - done, cnt;
        if (!off && left >= HDF_BLOCK && !((uintptr_t)p & 3)) {
            cnt = left / HDF_BLOCK;                         /* straight into the caller's buffer */
            if (hdf_read(v->io, v->f, blk, cnt, p)) return done ? (int32_t)done : ISO_IO;
            cnt *= HDF_BLOCK;
        } else {
            if (hdf_read(v->io, v->f, blk, 1, v->bounce)) return done ? (int32_t)done : ISO_IO;
            cnt = HDF_BLOCK - off < left ? HDF_BLOCK - off : left;
            copy(p, v->bounce + off, cnt);
        }
        p += cnt; pos += cnt; done += cnt;
    }
    return (int32_t)done;
}
