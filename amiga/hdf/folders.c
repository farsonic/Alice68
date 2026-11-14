/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice68hdf.device: folders of the SD card's FAT partition as Amiga volumes.
 *
 * config.txt (the Pi's, on the same partition) names them:
 *     alice68.folder=Games              volume Games:
 *     alice68.folder=Amiga/Work:Work    the folder Amiga/Work as volume Work:
 * and a folder named ALICE68 in the root (Alice68's own Amiga files and their
 * installer) is always mounted as ALICE68:, unless alice68.folders=off.
 * They are read-write (read-only with hdf.readonly or a read-only card), never
 * bootable, and are DOS devices SDF0:, SDF1:, ... A folder not on the FAT partition is
 * looked for on the card's exFAT partition, and that partition is a volume itself (named
 * by its label); both read-only unless config.txt has alice68.exfat=rw (alice68.exfat=off:
 * not the partition itself).
 *
 * Each is a DOS handler process started by dos.library at boot (the boot
 * node's ADNF_STARTPROC). The code is here in the ROM; dos.library is given a
 * small seglist in RAM that calls folder_main(). All the folders share one FAT
 * engine (fatfs.c) and its caches, behind a semaphore; each process does its
 * own card I/O. Hardfiles that are mounted (alice68hdf.device units) are
 * read-only through a folder: their clusters are in use by the unit.
 * A folder without a Disk.info of its own shows Alice68's SD folder icon
 * (disk_icons.h): read-only, until a real one is written (a Workbench Snapshot).
 * Amiga attributes FAT cannot hold (comments, flags but ----rwed / ----r-e-, exact
 * dates) and names it cannot hold are kept as FS-UAE and Amiberry keep them
 * (uaem.h): NAME.uaem sidecars and %xx names; sidecars and macOS/Windows clutter
 * are hidden.
 */
#include "hdf_dev.h"

#include "dosdefs.h"
#include "disk_icons.h"
#include "uaem.h"

#define FOLDER_STACK 16384
extern void folder_main(struct HdfBase *db);

/* ---- the volumes ------------------------------------------------------------------ */
struct FVol {                               /* starts as struct FVolHead (hdf_dev.h): Alice68-ShowConfig reads it */
    ULONG index;
    char  path[128];                        /* on the FAT partition */
    char  vol[32];                          /* volume name */
    char  dev[8];                           /* DOS device: SDF0 ... */
    ULONG root;                             /* the folder's first cluster */
    struct ffs_ent rent;                    /* its own entry, for its dates */
    struct DosList *dl;                     /* the volume node, once the handler runs */
    struct ffs *fs;                         /* its partition's engine: FAT (db->ffs) or exFAT (db->xffs) */
    ULONG ro;                               /* read-only (exFAT, until it can be written) */
};

ACHECK(AOFF(struct FVol, path) == AOFF(struct FVolHead, path));
ACHECK(AOFF(struct FVol, vol) == AOFF(struct FVolHead, vol));
ACHECK(AOFF(struct FVol, dev) == AOFF(struct FVolHead, dev));

/* a lock or an open file */
struct FObj {
    struct FileLock fl;                     /* first: a lock's BPTR points here */
    struct FObj *next;
    struct FVol *fv;
    ULONG isfile;                           /* an open file (else a lock) */
    ULONG isroot;                           /* the volume's root */
    ULONG hdf;                              /* a mounted hardfile: never written */
    ULONG virt;                             /* the built-in Disk.info (no entry on the card) */
    ULONG written;                          /* changed since it was opened: a sidecar's date follows */
    struct ffs_file h;                      /* the object's entry; files: the open file */
    ULONG pos;
};

/* a sidecar in the directory being listed */
struct MRef {
    ULONG hash;                             /* of its file's host name, case folded */
    ULONG first, size;
    UBYTE nofat;
    uint64_t xsize, xvalid;
};

/* one handler process */
struct FCtx {
    struct HdfBase *db;
    struct FVol *fv;
    struct card c;
    struct MsgPort *cport;
    struct hdf_io io;
    APTR dos;
    struct MsgPort *port;
    /* the sidecars of one directory, found in one pass (a listing asks for each entry's) */
    struct MRef *mref;
    ULONG nmref, capmref, mdir, mgen, mvalid;
    ULONG sdir, sdir_valid, sdir_is, sdir_gen;   /* the last directory asked about: is it named S? */
    struct DateStamp now;                   /* when the packet being done came */
};

static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, uint32_t max) { uint32_t k = 0; for (; s[k] && k + 1 < max; k++) d[k] = s[k]; d[k] = 0; }

static LONG dos_err(int r)
{
    switch (r) {
    case FFS_END:      return ERROR_OBJECT_NOT_FOUND;
    case FFS_EXISTS:   return ERROR_OBJECT_EXISTS;
    case FFS_FULL: case FFS_DIRFULL: return ERROR_DISK_FULL;
    case FFS_NOTEMPTY: return ERROR_DIRECTORY_NOT_EMPTY;
    case FFS_BADNAME:  return ERROR_INVALID_COMPONENT_NAME;
    case FFS_WP:       return ERROR_DISK_WRITE_PROTECTED;
    case FFS_ISDIR: case FFS_NOTDIR: return ERROR_OBJECT_WRONG_TYPE;
    case FFS_LOOP:     return ERROR_OBJECT_IN_USE;
    case FFS_CORRUPT:  return ERROR_NOT_A_DOS_DISK;
    default:           return ERROR_SEEK_ERROR;
    }
}

/* ---- objects ------------------------------------------------------------------------ */
static int is_dir(const struct FObj *o) { return o->isroot || (o->h.e.attr & FFS_A_DIR); }
static uint32_t dir_key(const struct FObj *o) { return o->isroot ? o->fv->root : o->h.e.first; }

/* a path's end point (or, when it is not there, where it would be made) */
struct where {
    int isroot;
    struct ffs_ent e;
    uint32_t dir;                           /* the directory it is (to be) in */
    const char *leaf;                       /* the last name, if it was not found */
    uint32_t leaflen;
    int virt;                               /* the built-in Disk.info (leaf and dir are set too) */
    char host[FFS_NAME];                    /* the leaf's name on the card (%xx for what it cannot hold) */
};

/* the built-in Disk.info, in the volume's root while it has none of its own */
#define VIRT_SEC 0xffffffffu                /* no card sector: never the same as a real entry */
#define VIRT_POS 0xfffffff0u                /* ExNext: it was the last entry */
static void virt_ent(struct FCtx *x, struct ffs_ent *e)
{
    memset(e, 0, sizeof *e);
    e->dir = x->fv->root;
    e->sec = VIRT_SEC;
    e->attr = FFS_A_RO | FFS_A_ARCH;
    e->size = disk_icon_folder_len;
    e->date = x->fv->rent.date; e->time = x->fv->rent.time;
    scpy(e->name, DISK_ICON_NAME, sizeof e->name);
    scpy(e->sfn, "DISK.INF", sizeof e->sfn);
}

static int same(int adir, uint32_t akey, const struct ffs_ent *ae, const struct FObj *b)
{
    if (adir != is_dir(b)) return 0;
    if (adir) return akey == dir_key(b);
    return ae->sec == b->h.e.sec && ae->slot == b->h.e.slot;
}
static int w_isdir(const struct FCtx *x, const struct where *w) { (void)x; return w->isroot || (w->e.attr & FFS_A_DIR); }
static uint32_t w_key(const struct FCtx *x, const struct where *w) { return w->isroot ? x->fv->root : w->e.first; }

/* 0: free to take; else ERROR_OBJECT_IN_USE */
static LONG conflict(struct FCtx *x, const struct where *w, int excl, const struct FObj *skip)
{
    int d = w_isdir(x, w);
    uint32_t k = w_key(x, w);
    for (struct FObj *o = x->db->objs; o; o = o->next) {
        if (o == skip || !same(d, k, &w->e, o)) continue;
        if (excl || o->fl.fl_Access == EXCLUSIVE_LOCK) return ERROR_OBJECT_IN_USE;
    }
    return 0;
}
static int in_use(struct FCtx *x, const struct where *w) { return conflict(x, w, 1, 0) != 0; }

/* a mounted hardfile or CD image: its first cluster is a unit's or a CD volume's first block */
static int hdf_mounted(struct FCtx *x, const struct ffs_ent *e)
{
    if ((e->attr & FFS_A_DIR) || !e->first) return 0;
    uint32_t sec = ffs_clus_sec(x->fv->fs, e->first);
    for (ULONG i = 0; i < x->db->nunits; i++) {
        struct HUnit *u = x->db->unit[i];
        if (u && u->f.next && u->f.ext[0].card_sec == sec) return 1;
    }
    for (ULONG i = 0; i < x->db->nivols; i++) {
        struct IVolHead *v = (struct IVolHead *)x->db->ivol[i];
        if (v && v->f.next && v->f.ext[0].card_sec == sec) return 1;
    }
    return 0;
}

static struct FObj *new_obj(struct FCtx *x, const struct where *w, LONG access, int isfile)
{
    struct FObj *o = AllocMem(sizeof *o, MEMF_PUBLIC | MEMF_CLEAR);
    if (!o) return 0;
    o->fv = x->fv;
    o->isroot = (ULONG)w->isroot;
    o->isfile = (ULONG)isfile;
    if (w->isroot) o->h.e = x->fv->rent;
    ffs_open(&o->h, w->isroot ? &x->fv->rent : &w->e);
    o->hdf = (ULONG)hdf_mounted(x, &o->h.e);
    o->virt = (ULONG)w->virt;
    o->fl.fl_Key = (LONG)(is_dir(o) ? dir_key(o) : o->h.e.sec * 16 + o->h.e.slot);
    o->fl.fl_Access = access == EXCLUSIVE_LOCK ? EXCLUSIVE_LOCK : SHARED_LOCK;
    o->fl.fl_Task = x->port;
    o->fl.fl_Volume = MKBADDR(x->fv->dl);
    o->next = x->db->objs;
    x->db->objs = o;
    return o;
}
static void free_obj(struct FCtx *x, struct FObj *o)
{
    for (struct FObj **p = &x->db->objs; *p; p = &(*p)->next)
        if (*p == o) { *p = o->next; break; }
    FreeMem(o, sizeof *o);
}
static struct FObj *lock_obj(struct FCtx *x, LONG bptr)
{
    struct FObj *o = BADDR(bptr);
    for (struct FObj *p = x->db->objs; p; p = p->next) if (p == o) return o->fl.fl_Task == x->port ? o : 0;
    return 0;
}

/* other open files of the same file: they see the new size and clusters */
static void share(struct FCtx *x, struct FObj *o)
{
    for (struct FObj *p = x->db->objs; p; p = p->next) {
        if (p == o || is_dir(p) || p->h.e.sec != o->h.e.sec || p->h.e.slot != o->h.e.slot) continue;
        p->h.e.size = o->h.e.size; p->h.e.first = o->h.e.first;
        p->h.e.date = o->h.e.date; p->h.e.time = o->h.e.time;
        p->h.ci = ~0u;
        if (p->pos > p->h.e.size) p->pos = p->h.e.size;
    }
}

/* ---- Amiga attributes in NAME.uaem sidecars (uaem.h) ------------------------------------- */
static ULONG fold_hash(const char *s, uint32_t n)
{
    ULONG h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        UBYTE c = (UBYTE)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) c = (UBYTE)(c - 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}
/* the sidecars of directory dir, in one pass */
static int mref_load(struct FCtx *x, uint32_t dir)
{
    if (x->mvalid && x->mdir == dir && x->mgen == x->db->mgen) return 0;
    x->nmref = 0; x->mvalid = 0;
    uint32_t pos = 0;
    struct ffs_ent e;
    while (ffs_next(x->fv->fs, dir, &pos, &e) == 0) {
        uint32_t n = slen(e.name);
        if ((e.attr & FFS_A_DIR) || n < 6 || e.size > 4096 || (e.name[0] == '.' && e.name[1] == '_')) continue;
        if (!ffs_name_eq(".uaem", e.name + n - 5, 5)) continue;
        if (x->nmref == x->capmref) {
            ULONG cap = x->capmref ? x->capmref * 2 : 32;
            struct MRef *m = AllocMem(cap * sizeof *m, MEMF_PUBLIC);
            if (!m) return -1;
            if (x->mref) { memcpy(m, x->mref, x->nmref * sizeof *m); FreeMem(x->mref, x->capmref * sizeof *m); }
            x->mref = m; x->capmref = cap;
        }
        struct MRef *r = &x->mref[x->nmref++];
        r->hash = fold_hash(e.name, n - 5);
        r->first = e.first; r->size = e.size; r->nofat = e.xnofat; r->xsize = e.xsize; r->xvalid = e.xvalid;
    }
    x->mdir = dir; x->mgen = x->db->mgen; x->mvalid = 1;
    return 0;
}
/* the attributes the sidecar of host in dir gives: 1 found, 0 none */
static int meta_get(struct FCtx *x, uint32_t dir, const char *host, struct uaem *m)
{
    if (mref_load(x, dir)) return 0;
    ULONG h = fold_hash(host, slen(host));
    for (ULONG i = 0; i < x->nmref; i++) {
        struct MRef *r = &x->mref[i];
        if (r->hash != h) continue;
        struct ffs_ent e;
        memset(&e, 0, sizeof e);
        e.first = r->first; e.size = r->size; e.xnofat = r->nofat; e.xsize = r->xsize; e.xvalid = r->xvalid;
        struct ffs_file f;
        ffs_open(&f, &e);
        UBYTE line[UAEM_LINE];
        int32_t n = ffs_read(x->fv->fs, &f, 0, line, sizeof line);
        return n > 0 && uaem_parse(line, (uint32_t)n, m) == 0;
    }
    return 0;
}
/* write (or, with remove, delete) the sidecar of host in dir; 0 or an FFS_ error */
static int meta_put(struct FCtx *x, uint32_t dir, const char *host, const struct uaem *m, int remove)
{
    struct ffs *v = x->fv->fs;
    char sc[FFS_NAME];
    struct ffs_ent e;
    x->db->mgen++;
    if (uaem_sidecar(host, sc, sizeof sc) < 0) return remove ? 0 : FFS_BADNAME;
    int r = ffs_find(v, dir, sc, slen(sc), &e);
    if (remove) return r == FFS_END ? 0 : r ? r : ffs_delete(v, &e);
    if (r == FFS_END) r = ffs_create(v, dir, sc, slen(sc), 0, &e);
    if (r) return r;
    char line[UAEM_LINE];
    uint32_t n = uaem_format(m, line);
    struct ffs_file f;
    ffs_open(&f, &e);
    int err = 0;
    if ((r = ffs_truncate(v, &f, 0))) return r;
    if (ffs_write(v, &f, 0, line, n, &err) != (int32_t)n) return err ? err : FFS_FULL;
    return ffs_flush_file(v, &f);
}
/* a directory named S (not a volume's root): its files are scripts unless a sidecar says
   otherwise, as AmigaOS needs for S:Startup-Sequence and the like copied from a PC */
static int in_s(struct FCtx *x, uint32_t dir)
{
    if (dir == x->fv->root) return 0;
    if (x->sdir_valid && x->sdir == dir && x->sdir_gen == x->db->mgen) return (int)x->sdir_is;
    struct ffs_ent de;
    char an[FFS_NAME];
    x->sdir = dir; x->sdir_valid = 1; x->sdir_is = 0; x->sdir_gen = x->db->mgen;
    if (ffs_dir_ent(x->fv->fs, dir, &de) == 0) { uaem_decode(de.name, an, sizeof an); x->sdir_is = (an[0] == 'S' || an[0] == 's') && !an[1]; }
    return (int)x->sdir_is;
}
/* what the Amiga sees of an entry with no sidecar */
static void meta_plain(struct FCtx *x, const struct ffs_ent *e, struct uaem *m)
{
    m->prot = ((e->attr & FFS_A_RO) ? FIBF_WRITE | FIBF_DELETE : 0) | ((e->attr & FFS_A_ARCH) ? 0 : FIBF_ARCHIVE);
    if (x && !(e->attr & FFS_A_DIR) && e->sec != VIRT_SEC && in_s(x, e->dir)) m->prot |= FIBF_SCRIPT;
    ffs_fat_to_amiga(e->date, e->time, &m->days, &m->mins, &m->ticks);
    m->comment[0] = 0;
}

/* ---- paths --------------------------------------------------------------------------- */
static int start_at(struct FCtx *x, LONG lock, struct where *w)
{
    w->leaf = 0;
    w->virt = 0;
    if (!lock) { w->isroot = 1; w->e = x->fv->rent; return 0; }
    struct FObj *o = lock_obj(x, lock);
    if (!o) return -ERROR_INVALID_LOCK;
    w->isroot = (int)o->isroot;
    w->virt = (int)o->virt;
    w->e = o->h.e;
    return 0;
}

static int up(struct FCtx *x, struct where *w)
{
    if (w->isroot) return -ERROR_OBJECT_NOT_FOUND;
    uint32_t p;
    if (!(w->e.attr & FFS_A_DIR)) p = w->e.dir;
    else { int r = ffs_parent(x->fv->fs, w->e.first, &p); if (r) return -dos_err(r); }
    if (p == x->fv->root) { w->isroot = 1; w->e = x->fv->rent; return 0; }
    int r = ffs_dir_ent(x->fv->fs, p, &w->e);
    return r ? -dos_err(r) : 0;
}

/* a BSTR name from lock: 0 found; 1 not found but its directory is (w->leaf); <0 -error */
static int resolve(struct FCtx *x, LONG lock, ULONG bname, struct where *w)
{
    int r = start_at(x, lock, w);
    if (r) return r;
    const UBYTE *b = BADDR(bname);
    uint32_t len = b ? b[0] : 0;
    const char *s = (const char *)b + 1;
    for (uint32_t i = 0; i < len; i++) if (s[i] == ':') { s += i + 1; len -= i + 1; i = ~0u; }   /* "VOL:", "SDF0:" */
    uint32_t i = 0;
    while (i < len) {
        if (s[i] == '/') { if ((r = up(x, w))) return r; i++; continue; }
        uint32_t j = i;
        while (j < len && s[j] != '/') j++;
        if (!w_isdir(x, w)) return -ERROR_OBJECT_WRONG_TYPE;
        uint32_t d = w_key(x, w);
        struct ffs_ent e;
        int hl = uaem_encode(s + i, j - i, w->host, sizeof w->host);
        if (hl < 0) return -ERROR_INVALID_COMPONENT_NAME;
        r = ffs_find(x->fv->fs, d, w->host, (uint32_t)hl, &e);
        if (r == 0 && uaem_hidden(e.name)) r = FFS_END;          /* sidecars and clutter are not there */
        if (r == FFS_END) {
            if (j < len) return -ERROR_DIR_NOT_FOUND;
            w->dir = d; w->leaf = w->host; w->leaflen = (uint32_t)hl;
            if (d == x->fv->root && ffs_name_eq(DISK_ICON_NAME, s + i, j - i)) {   /* the built-in one */
                w->isroot = 0; w->virt = 1;
                virt_ent(x, &w->e);
                return 0;
            }
            return 1;
        }
        if (r) return -dos_err(r);
        w->isroot = 0;
        w->virt = 0;
        w->e = e;
        i = j < len ? j + 1 : j;
    }
    w->dir = w->isroot ? 0 : w->e.dir;
    return 0;
}

/* ---- packets ---------------------------------------------------------------------------- */
static void fill_fib(struct FCtx *x, struct FileInfoBlock *fib, int isroot, const struct ffs_ent *e)
{
    memset(fib, 0, sizeof *fib);
    char an[FFS_NAME];
    if (!isroot) uaem_decode(e->name, an, sizeof an);
    const char *n = isroot ? x->fv->vol : an;
    uint32_t k = slen(n);
    if (k > 106) k = 106;
    fib->fib_FileName[0] = (char)k;
    memcpy(fib->fib_FileName + 1, n, k);
    int dir = isroot || (e->attr & FFS_A_DIR);
    fib->fib_DirEntryType = fib->fib_EntryType = isroot ? ST_ROOT : dir ? ST_USERDIR : ST_FILE;
    fib->fib_Size = dir ? 0 : (LONG)e->size;
    fib->fib_NumBlocks = dir ? 1 : (LONG)((e->size + 511) / 512);
    struct uaem m, fat;
    meta_plain(x, e, &fat);
    if (isroot || e->sec == VIRT_SEC || !meta_get(x, e->dir, e->name, &m)) m = fat;
    else {
        /* the archive bit is FAT's (every write sets it); the date is the sidecar's (as in
           FS-UAE: a copy on a PC gives files new dates, the sidecar keeps the Amiga's), and an
           Amiga write to the file brings the sidecar's up to date (ACTION_END) */
        m.prot = (m.prot & ~(ULONG)FIBF_ARCHIVE) | (fat.prot & FIBF_ARCHIVE);
    }
    fib->fib_Protection = (LONG)m.prot;
    fib->fib_Date.ds_Days = (LONG)m.days; fib->fib_Date.ds_Minute = (LONG)m.mins; fib->fib_Date.ds_Tick = (LONG)m.ticks;
    uint32_t cl = slen(m.comment);
    fib->fib_Comment[0] = (char)cl;
    memcpy(fib->fib_Comment + 1, m.comment, cl);
    fib->fib_DiskKey = 0;
}

static void fill_info(struct FCtx *x, struct InfoData *id)
{
    memset(id, 0, sizeof *id);
    struct ffs *v = x->fv->fs;
    uint32_t fr = 0, spc = ffs_cluster_bytes(v) / HDF_BLOCK, nc = ffs_clusters(v);
    ffs_free_clusters(v, &fr);
    id->id_UnitNumber = (LONG)x->fv->index;
    id->id_DiskState = x->db->readonly || x->fv->ro ? ID_WRITE_PROTECTED : ID_VALIDATED;
    id->id_NumBlocks = (LONG)(nc * spc);
    id->id_NumBlocksUsed = (LONG)((nc - fr) * spc);
    id->id_BytesPerBlock = HDF_BLOCK;
    id->id_DiskType = ID_DOS_DISK;
    id->id_VolumeNode = MKBADDR(x->fv->dl);
    id->id_InUse = x->db->objs ? DOSTRUE : DOSFALSE;
}

/* returns Res1; *res2 the error */
static LONG do_packet(struct FCtx *x, struct DosPacket *p, LONG *res2)
{
    struct HdfBase *db = x->db;
    struct ffs *v = x->fv->fs;
    int ro = db->readonly || x->fv->ro;
    struct where w, w2;
    struct FObj *o;
    int r;
    LONG e;
    *res2 = 0;
#define FAIL(err) do { *res2 = (err); return DOSFALSE; } while (0)
#define RESOLVE(lock, name, wp) do { r = resolve(x, (lock), (ULONG)(name), (wp)); if (r < 0) FAIL(-r); } while (0)
#define WRITABLE() do { if (ro) FAIL(ERROR_DISK_WRITE_PROTECTED); } while (0)
    switch (p->dp_Type) {
    case ACTION_LOCATE_OBJECT:
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if ((e = conflict(x, &w, p->dp_Arg3 == EXCLUSIVE_LOCK, 0))) FAIL(e);
        if (!(o = new_obj(x, &w, p->dp_Arg3, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_COPY_DIR:
    case ACTION_COPY_DIR_FH:
        if (p->dp_Type == ACTION_COPY_DIR_FH) { o = (struct FObj *)p->dp_Arg1; w.isroot = (int)o->isroot; w.e = o->h.e; w.virt = (int)o->virt; }
        else if ((r = start_at(x, p->dp_Arg1, &w))) FAIL(-r);
        if ((e = conflict(x, &w, 0, 0))) FAIL(e);
        if (!(o = new_obj(x, &w, SHARED_LOCK, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_FREE_LOCK:
        if (p->dp_Arg1) { if (!(o = lock_obj(x, p->dp_Arg1))) FAIL(ERROR_INVALID_LOCK); free_obj(x, o); }
        return DOSTRUE;

    case ACTION_PARENT:
    case ACTION_PARENT_FH:
        if (p->dp_Type == ACTION_PARENT_FH) { o = (struct FObj *)p->dp_Arg1; w.isroot = 0; w.e = o->h.e; }
        else {
            if (!p->dp_Arg1) return 0;
            if ((r = start_at(x, p->dp_Arg1, &w))) FAIL(-r);
        }
        if (w.isroot) return 0;                       /* the root has none: 0, no error */
        if ((r = up(x, &w))) FAIL(-r);
        if ((e = conflict(x, &w, 0, 0))) FAIL(e);
        if (!(o = new_obj(x, &w, SHARED_LOCK, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_SAME_LOCK: {
        struct FObj *a = lock_obj(x, p->dp_Arg1), *b = lock_obj(x, p->dp_Arg2);
        if (!a || !b) FAIL(ERROR_INVALID_LOCK);
        return same(is_dir(a), dir_key(a), &a->h.e, b) ? DOSTRUE : DOSFALSE;
    }

    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_FH:
        if (p->dp_Type == ACTION_EXAMINE_FH) { o = (struct FObj *)p->dp_Arg1; w.isroot = (int)o->isroot; w.e = o->h.e; }
        else if ((r = start_at(x, p->dp_Arg1, &w))) FAIL(-r);
        fill_fib(x, BADDR(p->dp_Arg2), w.isroot, &w.e);
        return DOSTRUE;

    case ACTION_EXAMINE_NEXT: {
        struct FileInfoBlock *fib = BADDR(p->dp_Arg2);
        if ((r = start_at(x, p->dp_Arg1, &w))) FAIL(-r);
        if (!w_isdir(x, &w)) FAIL(ERROR_OBJECT_WRONG_TYPE);
        uint32_t pos = (uint32_t)fib->fib_DiskKey;
        struct ffs_ent n;
        if (pos == VIRT_POS) FAIL(ERROR_NO_MORE_ENTRIES);
        do r = ffs_next(v, w_key(x, &w), &pos, &n); while (r == 0 && uaem_hidden(n.name));
        if (r == FFS_END) {                            /* the root without a Disk.info: the built-in one last */
            if (w_key(x, &w) != x->fv->root || ffs_find(v, x->fv->root, DISK_ICON_NAME, sizeof DISK_ICON_NAME - 1, &n) != FFS_END)
                FAIL(ERROR_NO_MORE_ENTRIES);
            virt_ent(x, &n);
            fill_fib(x, fib, 0, &n);
            fib->fib_DiskKey = (LONG)VIRT_POS;
            return DOSTRUE;
        }
        if (r) FAIL(dos_err(r));
        fill_fib(x, fib, 0, &n);
        fib->fib_DiskKey = (LONG)pos;
        return DOSTRUE;
    }

    case ACTION_DISK_INFO:
        fill_info(x, BADDR(p->dp_Arg1));
        return DOSTRUE;
    case ACTION_INFO:
        if (p->dp_Arg1 && !lock_obj(x, p->dp_Arg1)) FAIL(ERROR_INVALID_LOCK);
        fill_info(x, BADDR(p->dp_Arg2));
        return DOSTRUE;

    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        struct FileHandle *fh = BADDR(p->dp_Arg1);
        int excl = p->dp_Type == ACTION_FINDOUTPUT;
        RESOLVE(p->dp_Arg2, p->dp_Arg3, &w);
        if (r == 0 && w.virt && p->dp_Type != ACTION_FINDINPUT) r = 1;     /* writing Disk.info: a real one */
        if (r == 0) {
            if (w_isdir(x, &w)) FAIL(ERROR_OBJECT_WRONG_TYPE);
            if ((e = conflict(x, &w, excl, 0))) FAIL(e);
            if (excl) {
                WRITABLE();
                if (hdf_mounted(x, &w.e)) FAIL(ERROR_OBJECT_IN_USE);
                if (w.e.attr & FFS_A_RO) FAIL(ERROR_WRITE_PROTECTED);
            }
        } else {
            if (p->dp_Type == ACTION_FINDINPUT) FAIL(ERROR_OBJECT_NOT_FOUND);
            WRITABLE();
            if ((r = ffs_create(v, w.dir, w.leaf, w.leaflen, 0, &w.e))) FAIL(dos_err(r));
            w.isroot = 0;
        }
        if (!(o = new_obj(x, &w, excl ? EXCLUSIVE_LOCK : SHARED_LOCK, 1))) FAIL(ERROR_NO_FREE_STORE);
        if (excl && o->h.e.size && (r = ffs_truncate(v, &o->h, 0))) { free_obj(x, o); FAIL(dos_err(r)); }
        fh->fh_Arg1 = (LONG)o;
        fh->fh_Port = 0;                               /* not interactive */
        return DOSTRUE;
    }

    case ACTION_FH_FROM_LOCK: {
        struct FileHandle *fh = BADDR(p->dp_Arg1);
        if (!(o = lock_obj(x, p->dp_Arg2))) FAIL(ERROR_INVALID_LOCK);
        if (is_dir(o)) FAIL(ERROR_OBJECT_WRONG_TYPE);
        o->isfile = 1; o->pos = 0;
        fh->fh_Arg1 = (LONG)o;
        fh->fh_Port = 0;
        return DOSTRUE;
    }

    case ACTION_END:
        o = (struct FObj *)p->dp_Arg1;
        if (!o->virt) ffs_flush_file(v, &o->h);
        if (o->written && !ro) {                       /* written: a sidecar's date follows the file */
            struct uaem md;
            if (meta_get(x, o->h.e.dir, o->h.e.name, &md)) {
                md.days = (uint32_t)x->now.ds_Days; md.mins = (uint32_t)x->now.ds_Minute; md.ticks = (uint32_t)x->now.ds_Tick;
                meta_put(x, o->h.e.dir, o->h.e.name, &md, 0);
            }
        }
        free_obj(x, o);
        return DOSTRUE;

    case ACTION_READ: {
        o = (struct FObj *)p->dp_Arg1;
        if (o->virt) {                                 /* the built-in Disk.info */
            ULONG k = o->pos < disk_icon_folder_len ? disk_icon_folder_len - o->pos : 0;
            if (k > (ULONG)p->dp_Arg3) k = (ULONG)p->dp_Arg3;
            memcpy((APTR)p->dp_Arg2, disk_icon_folder + o->pos, k);
            o->pos += k;
            return (LONG)k;
        }
        int32_t n = ffs_read(v, &o->h, o->pos, (APTR)p->dp_Arg2, (ULONG)p->dp_Arg3);
        if (n < 0) { *res2 = dos_err(n); return -1; }
        o->pos += (ULONG)n;
        return n;
    }

    case ACTION_WRITE: {
        o = (struct FObj *)p->dp_Arg1;
        if (ro) { *res2 = ERROR_DISK_WRITE_PROTECTED; return -1; }
        if (o->hdf) { *res2 = ERROR_OBJECT_IN_USE; return -1; }
        if (o->h.e.attr & FFS_A_RO) { *res2 = ERROR_WRITE_PROTECTED; return -1; }
        int err;
        int32_t n = ffs_write(v, &o->h, o->pos, (APTR)p->dp_Arg2, (ULONG)p->dp_Arg3, &err);
        o->pos += (ULONG)n;
        if (n > 0) o->written = 1;
        share(x, o);
        if (err && !n) { *res2 = dos_err(err); return -1; }
        if (err) *res2 = dos_err(err);
        return n;
    }

    case ACTION_SEEK: {
        o = (struct FObj *)p->dp_Arg1;
        LONG base = p->dp_Arg3 == OFFSET_BEGINNING ? 0 : p->dp_Arg3 == OFFSET_END ? (LONG)o->h.e.size : (LONG)o->pos;
        LONG np = base + p->dp_Arg2;
        if ((p->dp_Arg3 < -1 || p->dp_Arg3 > 1) || np < 0 || (ULONG)np > o->h.e.size) { *res2 = ERROR_SEEK_ERROR; return -1; }
        LONG old = (LONG)o->pos;
        o->pos = (ULONG)np;
        return old;
    }

    case ACTION_SET_FILE_SIZE: {
        o = (struct FObj *)p->dp_Arg1;
        if (ro) { *res2 = ERROR_DISK_WRITE_PROTECTED; return -1; }
        if (o->hdf) { *res2 = ERROR_OBJECT_IN_USE; return -1; }
        if (o->virt) { *res2 = ERROR_WRITE_PROTECTED; return -1; }
        LONG base = p->dp_Arg3 == OFFSET_BEGINNING ? 0 : p->dp_Arg3 == OFFSET_END ? (LONG)o->h.e.size : (LONG)o->pos;
        LONG ns = base + p->dp_Arg2;
        if (ns < 0) { *res2 = ERROR_SEEK_ERROR; return -1; }
        if ((r = ffs_truncate(v, &o->h, (ULONG)ns))) { *res2 = dos_err(r); return -1; }
        o->written = 1;
        if (o->pos > o->h.e.size) o->pos = o->h.e.size;
        share(x, o);
        return ns;
    }

    case ACTION_DELETE_OBJECT:
        WRITABLE();
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.virt) FAIL(ERROR_DELETE_PROTECTED);
        if (w.isroot || in_use(x, &w) || hdf_mounted(x, &w.e)) FAIL(ERROR_OBJECT_IN_USE);
        if (w.e.attr & FFS_A_RO) FAIL(ERROR_DELETE_PROTECTED);
        {
            struct uaem m;
            if (meta_get(x, w.e.dir, w.e.name, &m) && (m.prot & FIBF_DELETE)) FAIL(ERROR_DELETE_PROTECTED);
        }
        if ((r = ffs_delete(v, &w.e))) FAIL(dos_err(r));
        meta_put(x, w.e.dir, w.e.name, 0, 1);         /* and its sidecar */
        return DOSTRUE;

    case ACTION_RENAME_OBJECT: {
        WRITABLE();
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.isroot) FAIL(ERROR_OBJECT_IN_USE);
        if (w.virt) FAIL(ERROR_WRITE_PROTECTED);
        if (p->dp_Arg3 && !lock_obj(x, p->dp_Arg3)) FAIL(ERROR_INVALID_LOCK);
        RESOLVE(p->dp_Arg3, p->dp_Arg4, &w2);
        if (r == 0 && w2.virt) r = 1;                 /* renaming a file to Disk.info: it becomes the real one */
        if (r == 0) {                                 /* there already: fine only for a new spelling of the same name */
            if (w2.isroot || w2.e.sec != w.e.sec || w2.e.slot != w.e.slot) FAIL(ERROR_OBJECT_EXISTS);
            const UBYTE *b = BADDR(p->dp_Arg4);       /* its last name, as given */
            uint32_t len = b[0], k = len;
            while (k && b[k] != '/' && b[k] != ':') k--;
            int hl = uaem_encode((const char *)b + k + 1, len - k, w2.host, sizeof w2.host);
            if (hl < 0) FAIL(ERROR_INVALID_COMPONENT_NAME);
            w2.dir = w.e.dir; w2.leaf = w2.host; w2.leaflen = (uint32_t)hl;
        }
        int dir = w_isdir(x, &w);
        uint32_t key = w_key(x, &w);
        struct ffs_ent old = w.e, ne;
        if ((r = ffs_rename(v, &w.e, w2.dir, w2.leaf, w2.leaflen, &ne))) FAIL(dos_err(r));
        db->mgen++;                                   /* names changed: what was learnt about them starts again */
        {                                             /* its sidecar goes with it */
            char sc[FFS_NAME], nsc[FFS_NAME];
            struct ffs_ent se, dummy;
            char nh[FFS_NAME];
            memcpy(nh, w2.leaf, w2.leaflen); nh[w2.leaflen] = 0;
            if (uaem_sidecar(old.name, sc, sizeof sc) >= 0 && ffs_find(v, old.dir, sc, slen(sc), &se) == 0) {
                if (uaem_sidecar(nh, nsc, sizeof nsc) >= 0) ffs_rename(v, &se, w2.dir, nsc, slen(nsc), &dummy);
                else ffs_delete(v, &se);
                db->mgen++;
            }
        }
        for (struct FObj *q = db->objs; q; q = q->next)       /* locks and files on it follow it */
            if (same(dir, key, &old, q)) { uint32_t sz = q->h.e.size, fi = q->h.e.first; q->h.e = ne; q->h.e.size = sz; q->h.e.first = fi; }
        return DOSTRUE;
    }

    case ACTION_CREATE_DIR:
        WRITABLE();
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r == 0 && !w.virt) FAIL(ERROR_OBJECT_EXISTS);
        if ((r = ffs_create(v, w.dir, w.leaf, w.leaflen, 1, &w.e))) FAIL(dos_err(r));
        w.isroot = 0;
        if (!(o = new_obj(x, &w, EXCLUSIVE_LOCK, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_SET_PROTECT:
    case ACTION_SET_DATE:
        WRITABLE();
        RESOLVE(p->dp_Arg2, p->dp_Arg3, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.isroot || w.virt) return DOSTRUE;       /* the folder's own entry is not the volume's to change */
        {
            struct uaem md;
            int have = meta_get(x, w.e.dir, w.e.name, &md);
            if (!have) meta_plain(x, &w.e, &md);
            if (p->dp_Type == ACTION_SET_PROTECT) {
                ULONG m = (ULONG)p->dp_Arg4;
                uint8_t a = (uint8_t)((w.e.attr & (FFS_A_HIDDEN | FFS_A_SYSTEM)) | ((m & FIBF_WRITE) ? FFS_A_RO : 0) | ((m & FIBF_ARCHIVE) ? 0 : FFS_A_ARCH));
                r = ffs_set_attr(v, &w.e, a);
                md.prot = m;
            } else {
                const struct DateStamp *ds = (const struct DateStamp *)p->dp_Arg4;
                uint16_t dd, tt;
                ffs_amiga_to_fat((uint32_t)ds->ds_Days, (uint32_t)ds->ds_Minute, (uint32_t)ds->ds_Tick, &dd, &tt);
                r = ffs_set_date(v, &w.e, dd, tt);
                md.days = (uint32_t)ds->ds_Days; md.mins = (uint32_t)ds->ds_Minute; md.ticks = (uint32_t)ds->ds_Tick;
            }
            if (r) FAIL(dos_err(r));
            /* a sidecar for what FAT cannot hold; none when it can hold it all */
            int s_default = !(w.e.attr & FFS_A_DIR) && in_s(x, w.e.dir);
            if (!uaem_plain(md.prot) || md.comment[0] || (s_default && !(md.prot & FIBF_SCRIPT))) r = meta_put(x, w.e.dir, w.e.name, &md, 0);
            else if (have) r = meta_put(x, w.e.dir, w.e.name, 0, 1);
            if (r) FAIL(dos_err(r));
        }
        for (struct FObj *q = db->objs; q; q = q->next)
            if (same(w_isdir(x, &w), w_key(x, &w), &w.e, q)) { q->h.e.attr = w.e.attr; q->h.e.date = w.e.date; q->h.e.time = w.e.time; }
        return DOSTRUE;

    case ACTION_SET_COMMENT: {                        /* in the sidecar */
        WRITABLE();
        RESOLVE(p->dp_Arg2, p->dp_Arg3, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.isroot || w.virt) return DOSTRUE;
        struct uaem md;
        int have = meta_get(x, w.e.dir, w.e.name, &md);
        if (!have) meta_plain(x, &w.e, &md);
        const UBYTE *c = BADDR(p->dp_Arg4);
        uint32_t cl = c ? c[0] : 0;
        if (cl > UAEM_COMMENT - 1) FAIL(ERROR_COMMENT_TOO_BIG);
        memcpy(md.comment, c + 1, cl); md.comment[cl] = 0;
        for (uint32_t i = 0; i < cl; i++) if ((UBYTE)md.comment[i] < 32) md.comment[i] = ' ';   /* one line */
        int s_default = !(w.e.attr & FFS_A_DIR) && in_s(x, w.e.dir);
        if (cl || !uaem_plain(md.prot) || (s_default && !(md.prot & FIBF_SCRIPT))) r = meta_put(x, w.e.dir, w.e.name, &md, 0);
        else r = have ? meta_put(x, w.e.dir, w.e.name, 0, 1) : 0;
        if (r) FAIL(dos_err(r));
        return DOSTRUE;
    }
    case ACTION_SET_OWNER:                            /* FAT keeps no owner: accepted, so that copies work */
        RESOLVE(p->dp_Arg2, p->dp_Arg3, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        return DOSTRUE;

    case ACTION_CHANGE_MODE: {
        o = p->dp_Arg1 == 0 ? lock_obj(x, p->dp_Arg2) : (struct FObj *)((struct FileHandle *)BADDR(p->dp_Arg2))->fh_Arg1;
        if (!o) FAIL(ERROR_INVALID_LOCK);
        int excl = p->dp_Arg3 == EXCLUSIVE_LOCK || p->dp_Arg3 == MODE_NEWFILE;
        w.isroot = (int)o->isroot; w.e = o->h.e;
        if ((e = conflict(x, &w, excl, o))) FAIL(e);
        o->fl.fl_Access = excl ? EXCLUSIVE_LOCK : SHARED_LOCK;
        return DOSTRUE;
    }

    case ACTION_CURRENT_VOLUME:
        return (LONG)MKBADDR(x->fv->dl);
    case ACTION_IS_FILESYSTEM:
    case ACTION_FLUSH:
    case ACTION_INHIBIT:
        return DOSTRUE;
    default:
        FAIL(ERROR_ACTION_NOT_KNOWN);
    }
#undef FAIL
#undef RESOLVE
#undef WRITABLE
}

/* one packet, with the FAT engine to itself; for folder_main and the 68k test harness */
LONG fold_do(struct FCtx *x, struct DosPacket *p)
{
    struct HdfBase *db = x->db;
    ObtainSemaphore(&db->fsem);
    struct ffs *v = x->fv->fs;
    ffs_set_io(v, &x->io);                             /* this process's card requests */
    DateStamp(x->dos, &x->now);
    ffs_amiga_to_fat((uint32_t)x->now.ds_Days, (uint32_t)x->now.ds_Minute, (uint32_t)x->now.ds_Tick, &v->date, &v->time);
    LONG res2;
    LONG res1 = do_packet(x, p, &res2);
    int e = ffs_sync(v);
    if (e == FFS_WP && !db->readonly) {
        db->readonly = 1;
        hdf_log(&x->io, "the card refused a write: %s unit 0 is read-only (dtoverlay=emmc,unit0=rw)", db->card_dev);
    }
    if (e && !res2) { res2 = dos_err(e); if (res1 == DOSTRUE) res1 = DOSFALSE; }
    ReleaseSemaphore(&db->fsem);
    p->dp_Res1 = res1;
    p->dp_Res2 = res2;
    return res1;
}

/* the handler's start: card, dos.library, the volume node. 0 = cannot run */
struct FCtx *fold_start(struct HdfBase *db, ULONG unit, struct MsgPort *port)
{
    if (unit < FOLDER_UNIT0 || unit - FOLDER_UNIT0 >= db->nfvols) return 0;
    struct FVol *fv = db->fvol[unit - FOLDER_UNIT0];
    struct FCtx *x = AllocMem(sizeof *x, MEMF_PUBLIC | MEMF_CLEAR);
    if (!x) return 0;
    x->db = db; x->fv = fv; x->port = port;
    struct hdf_io io = HDF_IO(&x->c);
    x->io = io;
    x->dos = OpenLibrary("dos.library", 36);
    if (!x->dos || card_open(db, &x->c, &x->cport)) {
        hdf_log(&x->io, "%s: cannot start (dos.library or %s)", fv->vol, db->card_dev);
        if (x->dos) CloseLibrary(x->dos);
        FreeMem(x, sizeof *x);
        return 0;
    }
    uint32_t n = slen(fv->vol);
    struct DosList *dl = AllocMem(sizeof *dl + 36, MEMF_PUBLIC | MEMF_CLEAR);
    if (dl) {
        UBYTE *name = (UBYTE *)(dl + 1);               /* longword-aligned BSTR after it */
        name[0] = (UBYTE)n;
        memcpy(name + 1, fv->vol, n);
        dl->dol_Type = DLT_VOLUME;
        dl->dol_Task = port;
        dl->dol_DiskType = ID_DOS_DISK;
        dl->dol_Name = MKBADDR(name);
        uint32_t d, m, t;
        ffs_fat_to_amiga(fv->rent.date, fv->rent.time, &d, &m, &t);
        dl->dol_VolumeDate.ds_Days = (LONG)d; dl->dol_VolumeDate.ds_Minute = (LONG)m; dl->dol_VolumeDate.ds_Tick = (LONG)t;
        LockDosList(x->dos, LDF_VOLUMES | LDF_WRITE);
        if (!AddDosEntry(x->dos, dl)) { FreeMem(dl, sizeof *dl + 36); dl = 0; }
        UnLockDosList(x->dos, LDF_VOLUMES | LDF_WRITE);
    }
    fv->dl = dl;
    if (!dl) hdf_log(&x->io, "%s: the volume could not be added", fv->vol);
    return x;
}

static void reply_pkt(struct DosPacket *p, struct MsgPort *me)
{
    struct MsgPort *to = p->dp_Port;
    struct Message *m = p->dp_Link;
    p->dp_Port = me;
    m->mn_Node.ln_Name = (char *)p;
    PutMsg(to, m);
}

/* a handler process (called from the RAM seglist folders_mount() made) */
void folder_main(struct HdfBase *db)
{
    struct MsgPort *port = (struct MsgPort *)((UBYTE *)FindTask(0) + 92);   /* pr_MsgPort */
    WaitPort(port);
    struct Message *m = GetMsg(port);
    struct DosPacket *p = (struct DosPacket *)m->mn_Node.ln_Name;
    struct FileSysStartupMsg *fssm = BADDR(p->dp_Arg2);
    struct DeviceNode *dn = BADDR(p->dp_Arg3);
    struct FCtx *x = fssm ? fold_start(db, fssm->fssm_Unit, port) : 0;
    if (!x) { p->dp_Res1 = DOSFALSE; p->dp_Res2 = ERROR_NO_FREE_STORE; reply_pkt(p, port); return; }
    dn->dn_Task = (ULONG)port;
    p->dp_Res1 = DOSTRUE; p->dp_Res2 = 0;
    reply_pkt(p, port);
    for (;;) {
        WaitPort(port);
        while ((m = GetMsg(port))) {
            p = (struct DosPacket *)m->mn_Node.ln_Name;
            fold_do(x, p);
            reply_pkt(p, port);
        }
    }
}

/* ---- at boot: which folders, and their boot nodes ------------------------------------------- */
APTR handler_seglist(struct HdfBase *db, void (*entry)(struct HdfBase *))
{
    ULONG *m = AllocMem(24, MEMF_PUBLIC | MEMF_CLEAR);
    if (!m) return 0;
    UWORD *c = (UWORD *)(m + 2);
    m[0] = 24;                                         /* (as LoadSeg leaves it) */
    m[1] = 0;                                          /* no next segment */
    c[0] = 0x4879; c[1] = (UWORD)((ULONG)db >> 16); c[2] = (UWORD)(ULONG)db;                  /* pea db */
    c[3] = 0x4eb9; c[4] = (UWORD)((ULONG)entry >> 16); c[5] = (UWORD)(ULONG)entry;            /* jsr entry */
    c[6] = 0x588f;                                     /* addq.l #4,sp */
    c[7] = 0x4e75;                                     /* rts */
    CacheClearU();
    return m + 1;
}

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* config.txt lines alice68.folder=PATH[:VOLUME] (and alice68.folders=off): calls take() for each */
/* alice68.exfat=off|ro|rw|boot: the exFAT partition as a volume (0 off, 1 read-only: the default,
   2 read-write, 3 read-write and bootable) */
static int exfat_mode(const char *c)
{
    static const char key[] = "alice68.exfat=";
    int mode = 1;
    while (c && *c) {
        const char *l = c;
        while (*c && *c != '\n') c++;
        const char *end = c;
        if (*c) c++;
        while (l < end && is_sp(*l)) l++;
        while (end > l && is_sp(end[-1])) end--;
        uint32_t i = 0;
        while (key[i] && l + i < end && l[i] == key[i]) i++;
        if (key[i]) continue;
        l += i;
        if (end - l == 2 && l[0] == 'r' && l[1] == 'w') mode = 2;
        else if (end - l == 2 && l[0] == 'r' && l[1] == 'o') mode = 1;
        else if (end - l == 3 && l[0] == 'o' && l[1] == 'f' && l[2] == 'f') mode = 0;
        else if (end - l == 4 && l[0] == 'b' && l[1] == 'o' && l[2] == 'o' && l[3] == 't') mode = 3;
    }
    return mode;
}

static int parse_config(const char *c, char paths[][128], char vols[][32], int max, int *off)
{
    int n = 0;
    *off = 0;
    while (c && *c) {
        const char *l = c;
        while (*c && *c != '\n') c++;
        const char *end = c;
        if (*c) c++;
        while (l < end && is_sp(*l)) l++;
        while (end > l && is_sp(end[-1])) end--;
        static const char key[] = "alice68.folder=", koff[] = "alice68.folders=off";
        uint32_t kl = sizeof key - 1, i = 0;
        if ((uint32_t)(end - l) == sizeof koff - 1) {
            while (i < sizeof koff - 1 && l[i] == koff[i]) i++;
            if (i == sizeof koff - 1) { *off = 1; continue; }
            i = 0;
        }
        while (i < kl && l + i < end && l[i] == key[i]) i++;
        if (i != kl || n >= max) continue;
        l += kl;
        const char *colon = 0;
        for (const char *q = l; q < end; q++) if (*q == ':') colon = q;
        const char *pe = colon ? colon : end;
        uint32_t k = 0;
        while (l < pe && (*l == '/' || *l == '\\')) l++;
        for (const char *q = l; q < pe && k < 127; q++) paths[n][k++] = *q == '\\' ? '/' : *q;
        while (k && paths[n][k - 1] == '/') k--;
        paths[n][k] = 0;
        k = 0;
        if (colon) for (const char *q = colon + 1; q < end && k < 30; q++) vols[n][k++] = *q;
        vols[n][k] = 0;
        if (paths[n][0]) n++;
    }
    return n;
}

/* a volume for a folder (path) of a partition, or (path "") the exFAT partition itself: 1 made,
   0 not (logged), -1 out of memory */
static int add_volume(struct HdfBase *db, struct hdf_io *io, APTR eb, APTR cd, const char *path, const char *volname,
                      struct ffs *fs, uint32_t root, const struct ffs_ent *re, int boot)
{
    const char *pth = path;
    struct FVol *fv = AllocMem(sizeof *fv, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fv) return -1;
    fv->fs = fs;
    fv->ro = fs == &db->xffs && db->xfat.ro != 0;  /* exFAT: read-only unless alice68.exfat=rw (and it may be) */
    fv->index = db->nfvols;
    scpy(fv->path, pth, sizeof fv->path);
    fv->root = root;
    fv->rent = *re;
    /* the volume name: given, else the folder's own name; never one mounted already */
    const char *base = volname[0] ? volname : re->name;
    scpy(fv->vol, base, 28);
    for (char d = '2'; d <= '9'; d++) {
        int clash = find_vol(db, fv->vol) >= 0;
        for (ULONG k = 0; k < db->nfvols && !clash; k++) clash = str_ieq(db->fvol[k]->vol, fv->vol);
        if (!clash) break;
        uint32_t l = slen(base) < 27 ? slen(base) : 27;
        memcpy(fv->vol, base, l); fv->vol[l] = d; fv->vol[l + 1] = 0;
    }
    scpy(fv->dev, "SDF0", sizeof fv->dev);
    fv->dev[3] = (char)('0' + fv->index);
    /* the seglist dos.library starts: in RAM, a call of folder_main(db) in the ROM */
    if (!db->fseg && !(db->fseg = handler_seglist(db, folder_main))) { FreeMem(fv, sizeof *fv); return -1; }
    LONG pri = -128;                                /* not bootable; the boot volume ahead of every other drive */
    if (boot) { LONG top = eb ? highest_boot_pri(eb) : -128; pri = top < 0 ? 0 : top + 1; if (pri > 127) pri = 127; }
    ULONG pkt[4 + 17] = { (ULONG)fv->dev, (ULONG)hdf_devname, FOLDER_UNIT0 + fv->index, 0,
                          16, 128, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0x1fe00, 0x7ffffffe, (ULONG)pri, 0x46415401 /* 'FAT\1' */ };
    struct DeviceNode *dn = eb ? MakeDosNode(eb, pkt) : 0;
    if (!dn) { hdf_log(io, "%s: MakeDosNode failed", fv->vol); FreeMem(fv, sizeof *fv); return 0; }
    dn->dn_SegList = MKBADDR(db->fseg);
    dn->dn_GlobalVec = (ULONG)-1;                   /* a C handler */
    dn->dn_StackSize = FOLDER_STACK;
    dn->dn_Priority = 10;
    dn->dn_Handler = 0;
    if (find_drive(eb, fv->dev)) hdf_log(io, "%s: a drive of that name is mounted already", fv->dev);
    db->fvol[db->nfvols++] = fv;
    if (!AddBootNode(eb, pri, 1 /* ADNF_STARTPROC */, dn, cd)) { hdf_log(io, "%s: AddBootNode failed", fv->vol); db->nfvols--; FreeMem(fv, sizeof *fv); return 0; }
    add_vol(db, fv->vol, fv->dev);
    if (fv->path[0])
        hdf_log(io, "folder %s%s: volume %s (%s:), not bootable%s", fv->path, fv->fs == &db->xffs ? " (exFAT)" : "", fv->vol, fv->dev,
                db->readonly || fv->ro ? ", read-only" : "");
    else
        hdf_log(io, "exFAT partition: volume %s (%s:), %s%s%s", fv->vol, fv->dev, boot ? "bootable" : "not bootable",
                db->readonly || fv->ro ? ", read-only: " : "", db->readonly ? "the card is" : fv->ro ? db->xfat.ro : "");
    if (boot) hdf_log(io, "exFAT partition: boots as %s (priority %ld)", fv->vol, pri);
    return 1;
}

void folders_mount(struct HdfBase *db, struct hdf_io *io, APTR eb, APTR cd, const char *config)
{
    char (*paths)[128] = AllocMem(FOLDER_MAX * (128 + 32), MEMF_PUBLIC | MEMF_CLEAR);
    if (!paths) return;
    char (*vols)[32] = (char (*)[32])(paths + FOLDER_MAX);
    int off;
    int n = 1 + parse_config(config, paths + 1, vols + 1, FOLDER_MAX - 1, &off);
    scpy(paths[0], "ALICE68", 128);                   /* Alice68's own files: always, if the folder is there */
    scpy(vols[0], "ALICE68", 32);
    if (off) { hdf_log(io, "folders turned off (alice68.folders=off)"); goto out; }
    InitSemaphore(&db->fsem);
    struct hdf_io quiet = *io;                        /* dev_init has said what the partition is */
    quiet.log = 0;
    if (fat_open(&db->ffat, &quiet) || (db->ffat.io = io, ffs_init(&db->ffs, &db->ffat))) { hdf_log(io, "folders: the FAT partition cannot be opened"); goto out; }
    db->ffs_up = 1;
    /* the card's exFAT partition: folders on it, and itself as a volume (alice68.exfat=) */
    int xm = exfat_mode(config);
    if (xfat_open(&db->xfat, &quiet) == 0) {
        if (ffs_init_exfat(&db->xffs, &db->xfat) == 0) {
            db->xffs_up = 1;
            ffs_set_io(&db->xffs, io);                 /* the validator's lines go to the log */
            if (xm >= 2) { const char *why = xfat_writable(&db->xfat); if (why) hdf_log(io, "exFAT partition: not written: %s", why); }
            else if (!db->xfat.ro) {
                if (db->xfat.flags & 2) hdf_log(io, "exFAT partition: not cleanly removed; with alice68.exfat=rw it is checked and repaired at boot");
                db->xfat.ro = "alice68.exfat=rw in config.txt writes it";
            }
        } else xfat_close(&db->xfat);
    }
    for (int i = 0; i < n && db->nfvols < FOLDER_MAX; i++) {
        char *pth = paths[i];
        while (*pth == '/') pth++;
        int dup = 0;                                   /* named twice (ALICE68 in config.txt too) */
        for (int k = 0; k < i; k++) if (str_ieq(paths[k], pth)) dup = 1;
        if (dup) continue;
        uint32_t root;
        struct ffs_ent re;
        struct ffs *fs = &db->ffs;
        int r = ffs_resolve(fs, pth, &root, &re);
        if (r == FFS_END) {                            /* not on the FAT partition: on the exFAT one? */
            if (db->xffs_up) {
                ffs_set_io(&db->xffs, io);
                int rx = ffs_resolve(&db->xffs, pth, &root, &re);
                if (rx != FFS_END) { r = rx; fs = &db->xffs; }
            }
        }
        if (r) {
            if (i) hdf_log(io, "config.txt: alice68.folder=%s: %s", pth, r == FFS_NOTDIR ? "not a folder" : r == FFS_BADNAME ? "not the root" : "no such folder");
            continue;
        }
        if (add_volume(db, io, eb, cd, pth, vols[i], fs, root, &re, 0) < 0) break;
    }
    if (db->xffs_up && xm && db->nfvols < FOLDER_MAX) {   /* the exFAT partition itself */
        struct ffs_ent re;
        memset(&re, 0, sizeof re);
        re.attr = FFS_A_DIR;
        re.first = re.dir = db->xfat.root;
        re.date = (uint16_t)((46 << 9) | (1 << 5) | 1);
        ffs_set_io(&db->xffs, io);
        int boot = xm == 3;
        if (boot && db->opts.boot[0]) { hdf_log(io, "alice68.exfat=boot: hdf.boot=%s boots instead", db->opts.boot); boot = 0; }
        add_volume(db, io, eb, cd, "", db->xfat.label[0] ? db->xfat.label : "exFAT", &db->xffs, db->xfat.root, &re, boot);
    }
out:
    FreeMem(paths, FOLDER_MAX * (128 + 32));
}
