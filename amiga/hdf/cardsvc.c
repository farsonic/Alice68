/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice68hdf.device: the card service (cardsvc.h) - the SD card for Alice68-Card.
 * Runs in the caller's process: its own card request, the FAT/exFAT engines under
 * db->fsem like the folder handlers, then ffs_sync before the answer.
 */
#include "dosdefs.h"
#include "cardsvc.h"

static int is_slash(char c) { return c == '/'; }
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, uint32_t max) { uint32_t i = 0; for (; s && s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }

static int cse(int r)
{
    switch (r) {
    case 0:            return 0;
    case FFS_END:      return CSE_NOTFOUND;
    case FFS_EXISTS:   return CSE_EXISTS;
    case FFS_FULL: case FFS_DIRFULL: return CSE_FULL;
    case FFS_NOTEMPTY: return CSE_NOTEMPTY;
    case FFS_BADNAME:  return CSE_BADNAME;
    case FFS_WP:       return CSE_RO;
    case FFS_ISDIR:    return CSE_ISDIR;
    case FFS_NOTDIR:   return CSE_NOTDIR;
    case FFS_LOOP:     return CSE_INUSE;
    default:           return CSE_IO;
    }
}

static uint32_t root_of(struct ffs *v) { return v->x ? v->x->root : 0; }

/* the folder a path names ("" or "/": the root) */
static int dir_of(struct ffs *v, const char *path, uint32_t *dir)
{
    const char *p = path;
    while (is_slash(*p)) p++;
    if (!*p) { *dir = root_of(v); return 0; }
    struct ffs_ent e;
    return ffs_resolve(v, path, dir, &e);
}

/* a path's folder and last name; 0, or an FFS_ error */
static int split(struct ffs *v, const char *path, uint32_t *dir, const char **leaf, uint32_t *len)
{
    uint32_t n = slen(path);
    while (n && is_slash(path[n - 1])) n--;                 /* "Games/" is Games */
    uint32_t k = n;
    while (k && !is_slash(path[k - 1])) k--;
    if (k == n) return FFS_BADNAME;                          /* no name: the root */
    *leaf = path + k;
    *len = n - k;
    if (!k) { *dir = root_of(v); return 0; }
    char parent[CS_PATH];
    uint32_t i = 0;
    for (; i + 1 < k && i < CS_PATH - 1; i++) parent[i] = path[i];
    parent[i] = 0;
    return dir_of(v, parent, dir);
}

static int find(struct ffs *v, const char *path, struct ffs_ent *e)
{
    uint32_t dir, len;
    const char *leaf;
    int r = split(v, path, &dir, &leaf, &len);
    return r ? r : ffs_find(v, dir, leaf, len, e);
}

/* ---- CS_INFO ----------------------------------------------------------------------- */
static int mcmp(const void *a, const void *b, uint32_t n) { const UBYTE *x = a, *y = b; while (n--) if (*x++ != *y++) return 1; return 0; }

static void label_of(const UBYTE *s, int fat32, char *out)
{
    const UBYTE *l = s + (fat32 ? 71 : 43);
    int n = 11;
    while (n && l[n - 1] == ' ') n--;
    int i = 0;
    for (; i < n && i < 15; i++) out[i] = (char)l[i];
    out[i] = 0;
    if (!out[0] || (n == 7 && !mcmp(out, "NO NAME", 7))) out[0] = 0;
}

static uint32_t free_kb(struct ffs *v)
{
    uint32_t n = 0;
    if (ffs_free_clusters(v, &n)) return ~0u;
    uint32_t cb = ffs_cluster_bytes(v);                 /* no 64-bit multiply here (no libgcc) */
    return cb >= 1024 ? n * (cb / 1024) : n / (1024 / (cb ? cb : 1024));
}

static void info(struct HdfBase *db, struct hdf_io *io, struct ffs *fat, struct cs_info *ci, int count_free)
{
    memset(ci, 0, sizeof *ci);
    ci->version = CS_VERSION;
    ci->card_ro = db->card_ro;
    ci->cmd64 = db->cmd64 != 0;
    scpy(ci->card_dev, db->card_dev, sizeof ci->card_dev);
    UBYTE *b = io->alloc(io->ctx, 512);
    if (!b) return;
    int bpb = 0, rd = io->read(io->ctx, 0, 1, b) == 0;
    if (rd && b[510] == 0x55 && b[511] == 0xaa &&
        (b[0] == 0xeb || b[0] == 0xe9) && b[11] == 0 && b[12] == 2 && b[13]) {           /* no partition table: one FAT from sector 0 */
        struct cs_part *p = &ci->part[ci->nparts++];
        uint32_t n = (uint32_t)b[19] | (uint32_t)b[20] << 8;
        if (!n) n = le32(b + 32);
        p->kind = n && !b[22] && !b[23] ? CSP_FAT32 : CSP_FAT16;
        p->count = n; p->total_kb = n / 2; p->free_kb = ~0u; p->vol = ~0u;
        label_of(b, p->kind == CSP_FAT32, p->label);
        bpb = 1;
    }
    if (rd && !bpb && b[510] == 0x55 && b[511] == 0xaa) {
        for (int i = 0; i < 4; i++) {
            const UBYTE *pe = b + 446 + 16 * i;
            if (!pe[4]) continue;
            struct cs_part *p = &ci->part[ci->nparts++];
            p->type = pe[4];
            p->boot = pe[0] == 0x80;
            p->start = le32(pe + 8);
            p->count = le32(pe + 12);
            p->total_kb = p->count / 2;
            p->free_kb = ~0u;
            p->vol = ~0u;
            switch (p->type) {
            case 0x01: case 0x04: case 0x06: case 0x0e: p->kind = CSP_FAT16; break;
            case 0x0b: case 0x0c: p->kind = CSP_FAT32; break;
            case 0x07: p->kind = CSP_OTHER; break;                     /* exFAT or NTFS: the boot sector says */
            case 0x76: p->kind = CSP_AMIGA; break;
            case 0x05: case 0x0f: case 0x85: p->kind = CSP_EXTENDED; break;
            default:   p->kind = CSP_OTHER; break;
            }
        }
        for (ULONG i = 0; i < ci->nparts; i++) {                       /* their boot sectors: kind, label */
            struct cs_part *p = &ci->part[i];
            if (p->kind == CSP_AMIGA || p->kind == CSP_EXTENDED || io->read(io->ctx, p->start, 1, b)) continue;
            if (!mcmp(b + 3, "EXFAT   ", 8)) p->kind = CSP_EXFAT;
            else if ((p->kind == CSP_FAT16 || p->kind == CSP_FAT32) && b[510] == 0x55) label_of(b, p->kind == CSP_FAT32, p->label);
        }
    }
    io->free(io->ctx, b, 512);
    /* which are Alice68's volumes */
    for (ULONG i = 0; i < ci->nparts; i++) {
        struct cs_part *p = &ci->part[i];
        if (fat && fat->f && p->start == fat->f->part_start) {
            p->vol = CS_FAT;
            if (count_free) p->free_kb = free_kb(fat);
        } else if (db->xffs_up && p->start == db->xfat.part_start) {
            p->vol = CS_EXFAT;
            scpy(p->label, db->xfat.label, sizeof p->label);
            if (count_free) p->free_kb = free_kb(&db->xffs);
        }
    }
    if (!db->xffs_up) scpy(ci->exfat_why, "no exFAT partition in use (none on the card, alice68.exfat=off, or folders off)", sizeof ci->exfat_why);
    else if (db->card_ro) scpy(ci->exfat_why, "the card is read-only", sizeof ci->exfat_why);
    else if (db->xfat.ro) scpy(ci->exfat_why, db->xfat.ro, sizeof ci->exfat_why);
    else ci->exfat_rw = 1;
    /* what is mounted from the card */
    for (ULONG i = 0; i < db->nunits && ci->nused < CS_MAXUSED; i++) {
        struct HUnit *u = db->unit[i];
        if (!u) continue;
        uint32_t sec = u->f.next ? u->f.ext[0].card_sec : 0;
        ci->used[ci->nused].kind = 1;
        ci->used[ci->nused].vol = db->xffs_up && sec >= db->xfat.part_start ? CS_EXFAT : CS_FAT;
        scpy(ci->used[ci->nused++].name, u->f.name, 64);
    }
    for (ULONG i = 0; i < db->nivols && ci->nused < CS_MAXUSED; i++) {
        struct IVolHead *v = (struct IVolHead *)db->ivol[i];
        if (!v) continue;
        uint32_t sec = v->f.next ? v->f.ext[0].card_sec : 0;
        ci->used[ci->nused].kind = 2;
        ci->used[ci->nused].vol = db->xffs_up && sec >= db->xfat.part_start ? CS_EXFAT : CS_FAT;
        scpy(ci->used[ci->nused++].name, v->f.name, 64);
    }
}

/* ---- the rest ---------------------------------------------------------------------- */
static int list(struct HdfBase *db, struct ffs *v, struct cs_list *l)
{
    uint32_t dir;
    int r = dir_of(v, l->path, &dir);
    if (r) return r;
    uint32_t pos = 0;
    struct ffs_ent e;
    l->count = l->more = 0;
    while ((r = ffs_next(v, dir, &pos, &e)) == 0) {
        if (e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2]))) continue;
        if (l->count >= l->max) { l->more = 1; break; }
        struct cs_ent *c = &l->ent[l->count++];
        scpy(c->name, e.name, sizeof c->name);
        if (v->x && !(e.attr & FFS_A_DIR)) { c->size = (uint32_t)e.xsize; c->sizehi = (uint32_t)(e.xsize >> 32); }
        else { c->size = (e.attr & FFS_A_DIR) ? 0 : e.size; c->sizehi = 0; }
        c->date = (uint32_t)e.date << 16 | e.time;
        c->flags = ((e.attr & FFS_A_DIR) ? CSF_DIR : 0) | ((e.attr & FFS_A_RO) ? CSF_RO : 0) | (fold_busy(db, v, &e) ? CSF_INUSE : 0);
    }
    return r == FFS_END ? 0 : r;
}

static int readf(struct ffs *v, struct cs_file *f, ULONG room, ULONG *actual)
{
    struct ffs_ent e;
    int r = find(v, f->path, &e);
    if (r) return r;
    if (e.attr & FFS_A_DIR) return FFS_ISDIR;
    f->len = e.size;
    if ((v->x && e.xsize > 0xffffffffull) || e.size > f->max || e.size > room) return -100;     /* CSE_TOOBIG */
    struct ffs_file h;
    ffs_open(&h, &e);
    int32_t n = ffs_read(v, &h, 0, f->data, e.size);
    if (n != (int32_t)e.size) return FFS_IO;
    *actual = (ULONG)n;
    return 0;
}

static int writef(struct HdfBase *db, struct ffs *v, struct cs_file *f, ULONG room, ULONG *actual)
{
    if (f->len > room) return -101;                                    /* IOERR_BADLENGTH */
    uint32_t dir, len;
    const char *leaf;
    int r = split(v, f->path, &dir, &leaf, &len);
    if (r) return r;
    struct ffs_ent e;
    r = ffs_find(v, dir, leaf, len, &e);
    if (r == 0) {
        if (e.attr & FFS_A_DIR) return FFS_ISDIR;
        if (fold_busy(db, v, &e)) return -102;                         /* CSE_INUSE */
    } else if (r == FFS_END) {
        if (!ffs_name_ok(leaf, len)) return FFS_BADNAME;
        r = ffs_create(v, dir, leaf, len, 0, &e);
        if (r) return r;
    } else return r;
    struct ffs_file h;
    int err = 0;
    ffs_open(&h, &e);
    if (f->len && ffs_write(v, &h, 0, f->data, f->len, &err) != (int32_t)f->len) return err ? err : FFS_IO;
    r = ffs_truncate(v, &h, f->len);
    if (!r) r = ffs_flush_file(v, &h);
    if (!r) *actual = f->len;
    return r;
}

static int del(struct HdfBase *db, struct ffs *v, struct cs_file *f)
{
    struct ffs_ent e;
    int r = find(v, f->path, &e);
    if (r) return r;
    if (fold_busy(db, v, &e)) return -102;
    return ffs_delete(v, &e);
}

static int ren(struct HdfBase *db, struct ffs *v, struct cs_file *f)
{
    struct ffs_ent e, out;
    int r = find(v, f->path, &e);
    if (r) return r;
    if (fold_busy(db, v, &e)) return -102;
    uint32_t ndir, len;
    const char *leaf;
    r = split(v, f->path2, &ndir, &leaf, &len);
    if (r) return r;
    if (!ffs_name_ok(leaf, len)) return FFS_BADNAME;
    return ffs_rename(v, &e, ndir, leaf, len, &out);
}

static int mkd(struct ffs *v, struct cs_file *f)
{
    uint32_t dir, len;
    const char *leaf;
    int r = split(v, f->path, &dir, &leaf, &len);
    if (r) return r;
    if (!ffs_name_ok(leaf, len)) return FFS_BADNAME;
    struct ffs_ent e;
    if (ffs_find(v, dir, leaf, len, &e) == 0) return FFS_EXISTS;
    return ffs_create(v, dir, leaf, len, 1, &e);
}

void card_service(struct HdfBase *db, struct IOStdReq *io)
{
    UWORD cmd = io->req.io_Command;
    io->io_Actual = 0;
    if (cmd < CS_INFO || cmd > CS_MKDIR) { io->req.io_Error = IOERR_NOCMD; return; }
    ULONG need = cmd == CS_INFO ? sizeof(struct cs_info) : cmd == CS_LIST ? sizeof(struct cs_list) : sizeof(struct cs_file) - 4;
    if (!io->io_Data || io->io_Length < need) { io->req.io_Error = IOERR_BADLENGTH; return; }
    struct card c;
    struct MsgPort *rp;
    c.db = db;
    if (card_open(db, &c, &rp)) { io->req.io_Error = CSE_IO; return; }
    struct hdf_io hio = HDF_IO(&c);
    hio.log = 0;                                         /* the engines' remarks stay out of the boot log */

    ObtainSemaphore(&db->fsem);
    /* the FAT partition: the folders' engine, or one just for this request */
    int temp = 0;
    struct ffs *fat = 0;
    if (db->ffs_up) fat = &db->ffs;
    else if (fat_open(&db->ffat, &hio) == 0) {
        if (ffs_init(&db->ffs, &db->ffat) == 0) { fat = &db->ffs; temp = 1; }
        else fat_close(&db->ffat);
    }
    if (fat) ffs_set_io(fat, &hio);
    if (db->xffs_up) ffs_set_io(&db->xffs, &hio);

    int r = 0, err = 0;
    ULONG vol = cmd == CS_INFO ? 0 : *(ULONG *)io->io_Data;
    struct ffs *v = cmd == CS_INFO ? 0 : vol == CS_FAT ? fat : vol == CS_EXFAT && db->xffs_up ? &db->xffs : 0;
    if (cmd == CS_INFO) info(db, &hio, fat, io->io_Data, io->io_Offset & 1);
    else if (!v) err = CSE_NOVOL;
    else {
        int writes = cmd >= CS_WRITE;
        if (writes && (db->card_ro || (v == &db->xffs && db->xfat.ro))) err = CSE_RO;
        else {
            /* "now", for the entries made or changed */
            APTR dos = OpenLibrary("dos.library", 36);
            if (dos) {
                struct DateStamp ds;
                DateStamp(dos, &ds);
                ffs_amiga_to_fat((uint32_t)ds.ds_Days, (uint32_t)ds.ds_Minute, (uint32_t)ds.ds_Tick, &v->date, &v->time);
                CloseLibrary(dos);
            }
            ULONG hdr = sizeof(struct cs_file) - 4, room = io->io_Length - hdr;
            struct cs_file *f = io->io_Data;
            struct cs_list *l = io->io_Data;
            switch (cmd) {
            case CS_LIST:
                if (io->io_Length < sizeof(struct cs_list) - sizeof(struct cs_ent) + l->max * sizeof(struct cs_ent)) err = IOERR_BADLENGTH;
                else { l->path[CS_PATH - 1] = 0; r = list(db, v, l); io->io_Actual = l->count; }
                break;
            case CS_READ:   f->path[CS_PATH - 1] = 0; r = readf(v, f, room, &io->io_Actual); break;
            case CS_WRITE:  f->path[CS_PATH - 1] = 0; r = writef(db, v, f, room, &io->io_Actual); break;
            case CS_DELETE: f->path[CS_PATH - 1] = 0; r = del(db, v, f); break;
            case CS_RENAME: f->path[CS_PATH - 1] = f->path2[CS_PATH - 1] = 0; r = ren(db, v, f); break;
            case CS_MKDIR:  f->path[CS_PATH - 1] = 0; r = mkd(v, f); break;
            }
            if (writes) db->mgen++;                      /* the folders' sidecar caches start again */
        }
    }
    if (r == -100) err = CSE_TOOBIG;
    else if (r == -101) err = IOERR_BADLENGTH;
    else if (r == -102) err = CSE_INUSE;
    else if (r && !err) err = cse(r);
    if (fat) { int s = ffs_sync(fat); if (s && !err) err = cse(s); }
    if (db->xffs_up) { int s = ffs_sync(&db->xffs); if (s && !err && v == &db->xffs) err = cse(s); }
    if (temp) { ffs_done(&db->ffs); fat_close(&db->ffat); }
    ReleaseSemaphore(&db->fsem);
    card_close(&c, rp);
    io->req.io_Error = (BYTE)err;
}
