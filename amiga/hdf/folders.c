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
 * bootable, and are DOS devices SDF0:, SDF1:, ...
 *
 * Each is a DOS handler process started by dos.library at boot (the boot
 * node's ADNF_STARTPROC). The code is here in the ROM; dos.library is given a
 * small seglist in RAM that calls folder_main(). All the folders share one FAT
 * engine (fatfs.c) and its caches, behind a semaphore; each process does its
 * own card I/O. Hardfiles that are mounted (alice68hdf.device units) are
 * read-only through a folder: their clusters are in use by the unit.
 */
#include "hdf_dev.h"

/* ---- dos.library ---------------------------------------------------------------- */
#define BADDR(b)  ((APTR)((ULONG)(b) << 2))
#define MKBADDR(p) ((ULONG)(p) >> 2)
#define DOSTRUE   (-1)
#define DOSFALSE  0

struct DateStamp { LONG ds_Days, ds_Minute, ds_Tick; };
struct DosPacket { struct Message *dp_Link; struct MsgPort *dp_Port; LONG dp_Type, dp_Res1, dp_Res2;
                   LONG dp_Arg1, dp_Arg2, dp_Arg3, dp_Arg4, dp_Arg5, dp_Arg6, dp_Arg7; };
struct FileLock { ULONG fl_Link; LONG fl_Key, fl_Access; struct MsgPort *fl_Task; ULONG fl_Volume; };
struct FileHandle { struct Message *fh_Link; struct MsgPort *fh_Port; struct MsgPort *fh_Type;
                    LONG fh_Buf, fh_Pos, fh_End, fh_Funcs, fh_Func2, fh_Func3, fh_Arg1, fh_Arg2; };
struct FileInfoBlock { LONG fib_DiskKey, fib_DirEntryType; char fib_FileName[108]; LONG fib_Protection, fib_EntryType, fib_Size, fib_NumBlocks;
                       struct DateStamp fib_Date; char fib_Comment[80]; UWORD fib_OwnerUID, fib_OwnerGID; char fib_Reserved[32]; };
struct InfoData { LONG id_NumSoftErrors, id_UnitNumber, id_DiskState, id_NumBlocks, id_NumBlocksUsed, id_BytesPerBlock, id_DiskType;
                  ULONG id_VolumeNode; LONG id_InUse; };
struct DosList { ULONG dol_Next; LONG dol_Type; struct MsgPort *dol_Task; ULONG dol_Lock;
                 struct DateStamp dol_VolumeDate; ULONG dol_LockList; LONG dol_DiskType, dol_pad; ULONG dol_Name; };   /* the volume side of its union */
struct FileSysStartupMsg { ULONG fssm_Unit, fssm_Device, fssm_Environ, fssm_Flags; };
ACHECK(sizeof(struct DosPacket) == 48);
ACHECK(AOFF(struct FileHandle, fh_Arg1) == 36);
ACHECK(AOFF(struct FileInfoBlock, fib_Protection) == 116);
ACHECK(AOFF(struct FileInfoBlock, fib_Date) == 132);
ACHECK(AOFF(struct FileInfoBlock, fib_OwnerUID) == 224);
ACHECK(sizeof(struct FileInfoBlock) == 260);
ACHECK(sizeof(struct InfoData) == 36);
ACHECK(AOFF(struct DosList, dol_Name) == 40);

#define ACTION_STARTUP        0
#define ACTION_DIE            5
#define ACTION_CURRENT_VOLUME 7
#define ACTION_LOCATE_OBJECT  8
#define ACTION_RENAME_DISK    9
#define ACTION_FREE_LOCK      15
#define ACTION_DELETE_OBJECT  16
#define ACTION_RENAME_OBJECT  17
#define ACTION_COPY_DIR       19
#define ACTION_SET_PROTECT    21
#define ACTION_CREATE_DIR     22
#define ACTION_EXAMINE_OBJECT 23
#define ACTION_EXAMINE_NEXT   24
#define ACTION_DISK_INFO      25
#define ACTION_INFO           26
#define ACTION_FLUSH          27
#define ACTION_SET_COMMENT    28
#define ACTION_PARENT         29
#define ACTION_INHIBIT        31
#define ACTION_SET_DATE       34
#define ACTION_SAME_LOCK      40
#define ACTION_READ           'R'
#define ACTION_WRITE          'W'
#define ACTION_FINDUPDATE     1004
#define ACTION_FINDINPUT      1005
#define ACTION_FINDOUTPUT     1006
#define ACTION_END            1007
#define ACTION_SEEK           1008
#define ACTION_SET_FILE_SIZE  1022
#define ACTION_WRITE_PROTECT  1023
#define ACTION_FH_FROM_LOCK   1026
#define ACTION_IS_FILESYSTEM  1027
#define ACTION_CHANGE_MODE    1028
#define ACTION_COPY_DIR_FH    1030
#define ACTION_PARENT_FH      1031
#define ACTION_EXAMINE_FH     1034
#define ACTION_SET_OWNER      1036

#define ERROR_NO_FREE_STORE        103
#define ERROR_OBJECT_IN_USE        202
#define ERROR_OBJECT_EXISTS        203
#define ERROR_DIR_NOT_FOUND        204
#define ERROR_OBJECT_NOT_FOUND     205
#define ERROR_ACTION_NOT_KNOWN     209
#define ERROR_INVALID_COMPONENT_NAME 210
#define ERROR_INVALID_LOCK         211
#define ERROR_OBJECT_WRONG_TYPE    212
#define ERROR_DISK_WRITE_PROTECTED 214
#define ERROR_DIRECTORY_NOT_EMPTY  216
#define ERROR_SEEK_ERROR           219
#define ERROR_DISK_FULL            221
#define ERROR_DELETE_PROTECTED     222
#define ERROR_WRITE_PROTECTED      223
#define ERROR_NOT_A_DOS_DISK       225
#define ERROR_NO_MORE_ENTRIES      232

#define SHARED_LOCK     (-2)
#define EXCLUSIVE_LOCK  (-1)
#define MODE_READWRITE  1004
#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT  0
#define OFFSET_END      1
#define ST_ROOT         1
#define ST_USERDIR      2
#define ST_FILE         (-3)
#define ID_WRITE_PROTECTED 80
#define ID_VALIDATED    82
#define ID_DOS_DISK     0x444f5300
#define DLT_VOLUME      2
#define LDF_WRITE       2
#define LDF_VOLUMES     8
#define FIBF_DELETE     1
#define FIBF_WRITE      4
#define FIBF_ARCHIVE    16

#define MEMF_PUBLIC 1
#define MEMF_CLEAR  0x10000
#define FOLDER_STACK 16384

static APTR AllocMem(ULONG n, ULONG req)          { return (APTR)lvo(SYSBASE, -198, n, req, 0, 0); }
static APTR FindTask(const char *n)               { return (APTR)lvo(SYSBASE, -294, 0, 0, 0, (ULONG)n); }
static void PutMsg(struct MsgPort *p, APTR m)     { lvo(SYSBASE, -366, 0, 0, (ULONG)p, (ULONG)m); }
static APTR GetMsg(struct MsgPort *p)             { return (APTR)lvo(SYSBASE, -372, 0, 0, (ULONG)p, 0); }
static APTR WaitPort(struct MsgPort *p)           { return (APTR)lvo(SYSBASE, -384, 0, 0, (ULONG)p, 0); }
static void InitSemaphore(struct SignalSemaphore *s)    { lvo(SYSBASE, -558, 0, 0, (ULONG)s, 0); }
static void ObtainSemaphore(struct SignalSemaphore *s)  { lvo(SYSBASE, -564, 0, 0, (ULONG)s, 0); }
static void ReleaseSemaphore(struct SignalSemaphore *s) { lvo(SYSBASE, -570, 0, 0, (ULONG)s, 0); }
static void CacheClearU(void)                     { lvo(SYSBASE, -636, 0, 0, 0, 0); }
static APTR MakeDosNode(APTR eb, ULONG *pkt)      { return (APTR)lvo(eb, -144, 0, 0, (ULONG)pkt, 0); }
static LONG AddBootNode(APTR eb, LONG pri, ULONG fl, APTR dn, APTR cd) { return (LONG)lvo(eb, -36, (ULONG)pri, fl, (ULONG)dn, (ULONG)cd); }
static void DateStamp(APTR dos, struct DateStamp *d) { lvo_d1(dos, -192, (ULONG)d); }
static APTR LockDosList(APTR dos, ULONG f)        { return (APTR)lvo_d1(dos, -654, f); }
static void UnLockDosList(APTR dos, ULONG f)      { lvo_d1(dos, -660, f); }
static LONG AddDosEntry(APTR dos, struct DosList *d) { return (LONG)lvo_d1(dos, -678, (ULONG)d); }
extern void folder_main(struct HdfBase *db);

/* ---- the volumes ------------------------------------------------------------------ */
struct FVol {
    ULONG index;
    char  path[128];                        /* on the FAT partition */
    char  vol[32];                          /* volume name */
    char  dev[8];                           /* DOS device: SDF0 ... */
    ULONG root;                             /* the folder's first cluster */
    struct ffs_ent rent;                    /* its own entry, for its dates */
    struct DosList *dl;                     /* the volume node, once the handler runs */
};

/* a lock or an open file */
struct FObj {
    struct FileLock fl;                     /* first: a lock's BPTR points here */
    struct FObj *next;
    struct FVol *fv;
    ULONG isfile;                           /* an open file (else a lock) */
    ULONG isroot;                           /* the volume's root */
    ULONG hdf;                              /* a mounted hardfile: never written */
    struct ffs_file h;                      /* the object's entry; files: the open file */
    ULONG pos;
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
};

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

/* a mounted hardfile: its first cluster is a unit's first block */
static int hdf_mounted(struct FCtx *x, const struct ffs_ent *e)
{
    struct fat *f = &x->db->ffat;
    if ((e->attr & FFS_A_DIR) || !e->first) return 0;
    uint32_t sec = f->data_start + (e->first - 2) * f->spc;
    for (ULONG i = 0; i < x->db->nunits; i++) {
        struct HUnit *u = x->db->unit[i];
        if (u && u->f.next && u->f.ext[0].card_sec == sec) return 1;
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

/* ---- paths --------------------------------------------------------------------------- */
static int start_at(struct FCtx *x, LONG lock, struct where *w)
{
    w->leaf = 0;
    if (!lock) { w->isroot = 1; w->e = x->fv->rent; return 0; }
    struct FObj *o = lock_obj(x, lock);
    if (!o) return -ERROR_INVALID_LOCK;
    w->isroot = (int)o->isroot;
    w->e = o->h.e;
    return 0;
}

static int up(struct FCtx *x, struct where *w)
{
    if (w->isroot) return -ERROR_OBJECT_NOT_FOUND;
    uint32_t p;
    if (!(w->e.attr & FFS_A_DIR)) p = w->e.dir;
    else { int r = ffs_parent(&x->db->ffs, w->e.first, &p); if (r) return -dos_err(r); }
    if (p == x->fv->root) { w->isroot = 1; w->e = x->fv->rent; return 0; }
    int r = ffs_dir_ent(&x->db->ffs, p, &w->e);
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
        r = ffs_find(&x->db->ffs, d, s + i, j - i, &e);
        if (r == FFS_END) {
            if (j < len) return -ERROR_DIR_NOT_FOUND;
            w->dir = d; w->leaf = s + i; w->leaflen = j - i;
            return 1;
        }
        if (r) return -dos_err(r);
        w->isroot = 0;
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
    const char *n = isroot ? x->fv->vol : e->name;
    uint32_t k = slen(n);
    if (k > 106) k = 106;
    fib->fib_FileName[0] = (char)k;
    memcpy(fib->fib_FileName + 1, n, k);
    int dir = isroot || (e->attr & FFS_A_DIR);
    fib->fib_DirEntryType = fib->fib_EntryType = isroot ? ST_ROOT : dir ? ST_USERDIR : ST_FILE;
    fib->fib_Size = dir ? 0 : (LONG)e->size;
    fib->fib_NumBlocks = dir ? 1 : (LONG)((e->size + 511) / 512);
    fib->fib_Protection = ((e->attr & FFS_A_RO) ? FIBF_WRITE | FIBF_DELETE : 0) | ((e->attr & FFS_A_ARCH) ? 0 : FIBF_ARCHIVE);
    uint32_t d, m, t;
    ffs_fat_to_amiga(e->date, e->time, &d, &m, &t);
    fib->fib_Date.ds_Days = (LONG)d; fib->fib_Date.ds_Minute = (LONG)m; fib->fib_Date.ds_Tick = (LONG)t;
    fib->fib_DiskKey = 0;
}

static void fill_info(struct FCtx *x, struct InfoData *id)
{
    memset(id, 0, sizeof *id);
    struct fat *f = &x->db->ffat;
    uint32_t fr = 0;
    ffs_free_clusters(&x->db->ffs, &fr);
    id->id_UnitNumber = (LONG)x->fv->index;
    id->id_DiskState = x->db->readonly ? ID_WRITE_PROTECTED : ID_VALIDATED;
    id->id_NumBlocks = (LONG)(f->clusters * f->spc);
    id->id_NumBlocksUsed = (LONG)((f->clusters - fr) * f->spc);
    id->id_BytesPerBlock = HDF_BLOCK;
    id->id_DiskType = ID_DOS_DISK;
    id->id_VolumeNode = MKBADDR(x->fv->dl);
    id->id_InUse = x->db->objs ? DOSTRUE : DOSFALSE;
}

/* returns Res1; *res2 the error */
static LONG do_packet(struct FCtx *x, struct DosPacket *p, LONG *res2)
{
    struct HdfBase *db = x->db;
    struct ffs *v = &db->ffs;
    struct where w, w2;
    struct FObj *o;
    int r;
    LONG e;
    *res2 = 0;
#define FAIL(err) do { *res2 = (err); return DOSFALSE; } while (0)
#define RESOLVE(lock, name, wp) do { r = resolve(x, (lock), (ULONG)(name), (wp)); if (r < 0) FAIL(-r); } while (0)
#define WRITABLE() do { if (db->readonly) FAIL(ERROR_DISK_WRITE_PROTECTED); } while (0)
    switch (p->dp_Type) {
    case ACTION_LOCATE_OBJECT:
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if ((e = conflict(x, &w, p->dp_Arg3 == EXCLUSIVE_LOCK, 0))) FAIL(e);
        if (!(o = new_obj(x, &w, p->dp_Arg3, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_COPY_DIR:
    case ACTION_COPY_DIR_FH:
        if (p->dp_Type == ACTION_COPY_DIR_FH) { o = (struct FObj *)p->dp_Arg1; w.isroot = (int)o->isroot; w.e = o->h.e; }
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
        r = ffs_next(v, w_key(x, &w), &pos, &n);
        if (r == FFS_END) FAIL(ERROR_NO_MORE_ENTRIES);
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
        ffs_flush_file(v, &o->h);
        free_obj(x, o);
        return DOSTRUE;

    case ACTION_READ: {
        o = (struct FObj *)p->dp_Arg1;
        int32_t n = ffs_read(v, &o->h, o->pos, (APTR)p->dp_Arg2, (ULONG)p->dp_Arg3);
        if (n < 0) { *res2 = dos_err(n); return -1; }
        o->pos += (ULONG)n;
        return n;
    }

    case ACTION_WRITE: {
        o = (struct FObj *)p->dp_Arg1;
        if (db->readonly) { *res2 = ERROR_DISK_WRITE_PROTECTED; return -1; }
        if (o->hdf) { *res2 = ERROR_OBJECT_IN_USE; return -1; }
        if (o->h.e.attr & FFS_A_RO) { *res2 = ERROR_WRITE_PROTECTED; return -1; }
        int err;
        int32_t n = ffs_write(v, &o->h, o->pos, (APTR)p->dp_Arg2, (ULONG)p->dp_Arg3, &err);
        o->pos += (ULONG)n;
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
        if (db->readonly) { *res2 = ERROR_DISK_WRITE_PROTECTED; return -1; }
        if (o->hdf) { *res2 = ERROR_OBJECT_IN_USE; return -1; }
        LONG base = p->dp_Arg3 == OFFSET_BEGINNING ? 0 : p->dp_Arg3 == OFFSET_END ? (LONG)o->h.e.size : (LONG)o->pos;
        LONG ns = base + p->dp_Arg2;
        if (ns < 0) { *res2 = ERROR_SEEK_ERROR; return -1; }
        if ((r = ffs_truncate(v, &o->h, (ULONG)ns))) { *res2 = dos_err(r); return -1; }
        if (o->pos > o->h.e.size) o->pos = o->h.e.size;
        share(x, o);
        return ns;
    }

    case ACTION_DELETE_OBJECT:
        WRITABLE();
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.isroot || in_use(x, &w) || hdf_mounted(x, &w.e)) FAIL(ERROR_OBJECT_IN_USE);
        if (w.e.attr & FFS_A_RO) FAIL(ERROR_DELETE_PROTECTED);
        if ((r = ffs_delete(v, &w.e))) FAIL(dos_err(r));
        return DOSTRUE;

    case ACTION_RENAME_OBJECT: {
        WRITABLE();
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.isroot) FAIL(ERROR_OBJECT_IN_USE);
        if (p->dp_Arg3 && !lock_obj(x, p->dp_Arg3)) FAIL(ERROR_INVALID_LOCK);
        RESOLVE(p->dp_Arg3, p->dp_Arg4, &w2);
        if (r == 0) {                                 /* there already: fine only for a new spelling of the same name */
            if (w2.isroot || w2.e.sec != w.e.sec || w2.e.slot != w.e.slot) FAIL(ERROR_OBJECT_EXISTS);
            const UBYTE *b = BADDR(p->dp_Arg4);       /* its last name, as given */
            uint32_t len = b[0], k = len;
            while (k && b[k] != '/' && b[k] != ':') k--;
            w2.dir = w.e.dir; w2.leaf = (const char *)b + k + 1; w2.leaflen = len - k;
        }
        int dir = w_isdir(x, &w);
        uint32_t key = w_key(x, &w);
        struct ffs_ent old = w.e, ne;
        if ((r = ffs_rename(v, &w.e, w2.dir, w2.leaf, w2.leaflen, &ne))) FAIL(dos_err(r));
        for (struct FObj *q = db->objs; q; q = q->next)       /* locks and files on it follow it */
            if (same(dir, key, &old, q)) { uint32_t sz = q->h.e.size, fi = q->h.e.first; q->h.e = ne; q->h.e.size = sz; q->h.e.first = fi; }
        return DOSTRUE;
    }

    case ACTION_CREATE_DIR:
        WRITABLE();
        RESOLVE(p->dp_Arg1, p->dp_Arg2, &w);
        if (r == 0) FAIL(ERROR_OBJECT_EXISTS);
        if ((r = ffs_create(v, w.dir, w.leaf, w.leaflen, 1, &w.e))) FAIL(dos_err(r));
        w.isroot = 0;
        if (!(o = new_obj(x, &w, EXCLUSIVE_LOCK, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_SET_PROTECT:
    case ACTION_SET_DATE:
        WRITABLE();
        RESOLVE(p->dp_Arg2, p->dp_Arg3, &w);
        if (r) FAIL(ERROR_OBJECT_NOT_FOUND);
        if (w.isroot) return DOSTRUE;                 /* the folder's own entry is not the volume's to change */
        if (p->dp_Type == ACTION_SET_PROTECT) {
            ULONG m = (ULONG)p->dp_Arg4;
            uint8_t a = (uint8_t)((w.e.attr & (FFS_A_HIDDEN | FFS_A_SYSTEM)) | ((m & FIBF_WRITE) ? FFS_A_RO : 0) | ((m & FIBF_ARCHIVE) ? 0 : FFS_A_ARCH));
            r = ffs_set_attr(v, &w.e, a);
        } else {
            const struct DateStamp *ds = (const struct DateStamp *)p->dp_Arg4;
            uint16_t dd, tt;
            ffs_amiga_to_fat((uint32_t)ds->ds_Days, (uint32_t)ds->ds_Minute, (uint32_t)ds->ds_Tick, &dd, &tt);
            r = ffs_set_date(v, &w.e, dd, tt);
        }
        if (r) FAIL(dos_err(r));
        for (struct FObj *q = db->objs; q; q = q->next)
            if (same(w_isdir(x, &w), w_key(x, &w), &w.e, q)) { q->h.e.attr = w.e.attr; q->h.e.date = w.e.date; q->h.e.time = w.e.time; }
        return DOSTRUE;

    case ACTION_SET_COMMENT:                          /* FAT keeps neither: accepted, so that copies work */
    case ACTION_SET_OWNER:
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
    db->ffat.io = &x->io;                              /* this process's card requests */
    struct DateStamp ds;
    DateStamp(x->dos, &ds);
    ffs_amiga_to_fat((uint32_t)ds.ds_Days, (uint32_t)ds.ds_Minute, (uint32_t)ds.ds_Tick, &db->ffs.date, &db->ffs.time);
    LONG res2;
    LONG res1 = do_packet(x, p, &res2);
    int e = ffs_sync(&db->ffs);
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
static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* config.txt lines alice68.folder=PATH[:VOLUME] (and alice68.folders=off): calls take() for each */
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
    for (int i = 0; i < n && db->nfvols < FOLDER_MAX; i++) {
        char *pth = paths[i];
        while (*pth == '/') pth++;
        int dup = 0;                                   /* named twice (ALICE68 in config.txt too) */
        for (int k = 0; k < i; k++) if (str_ieq(paths[k], pth)) dup = 1;
        if (dup) continue;
        uint32_t root;
        struct ffs_ent re;
        int r = ffs_resolve(&db->ffs, pth, &root, &re);
        if (r) {
            if (i) hdf_log(io, "config.txt: alice68.folder=%s: %s", pth, r == FFS_NOTDIR ? "not a folder" : r == FFS_BADNAME ? "not the root" : "no such folder");
            continue;
        }
        struct FVol *fv = AllocMem(sizeof *fv, MEMF_PUBLIC | MEMF_CLEAR);
        if (!fv) break;
        fv->index = db->nfvols;
        scpy(fv->path, pth, sizeof fv->path);
        fv->root = root;
        fv->rent = re;
        /* the volume name: given, else the folder's own name; never one mounted already */
        const char *base = vols[i][0] ? vols[i] : re.name;
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
        if (!db->fseg) {
            ULONG *m = AllocMem(24, MEMF_PUBLIC | MEMF_CLEAR);
            if (!m) { FreeMem(fv, sizeof *fv); break; }
            UWORD *c = (UWORD *)(m + 2);
            m[0] = 24;                                 /* (as LoadSeg leaves it) */
            m[1] = 0;                                  /* no next segment */
            c[0] = 0x4879; c[1] = (UWORD)((ULONG)db >> 16); c[2] = (UWORD)(ULONG)db;                          /* pea db */
            c[3] = 0x4eb9; c[4] = (UWORD)((ULONG)folder_main >> 16); c[5] = (UWORD)(ULONG)folder_main;      /* jsr folder_main */
            c[6] = 0x588f;                             /* addq.l #4,sp */
            c[7] = 0x4e75;                             /* rts */
            CacheClearU();
            db->fseg = m + 1;
        }
        ULONG pkt[4 + 17] = { (ULONG)fv->dev, (ULONG)hdf_devname, FOLDER_UNIT0 + fv->index, 0,
                              16, 128, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0x1fe00, 0x7ffffffe, (ULONG)-128, 0x46415401 /* 'FAT\1' */ };
        struct DeviceNode *dn = eb ? MakeDosNode(eb, pkt) : 0;
        if (!dn) { hdf_log(io, "%s: MakeDosNode failed", fv->vol); FreeMem(fv, sizeof *fv); continue; }
        dn->dn_SegList = MKBADDR(db->fseg);
        dn->dn_GlobalVec = (ULONG)-1;                   /* a C handler */
        dn->dn_StackSize = FOLDER_STACK;
        dn->dn_Priority = 10;
        dn->dn_Handler = 0;
        if (find_drive(eb, fv->dev)) hdf_log(io, "%s: a drive of that name is mounted already", fv->dev);
        db->fvol[db->nfvols++] = fv;
        if (!AddBootNode(eb, -128, 1 /* ADNF_STARTPROC */, dn, cd)) { hdf_log(io, "%s: AddBootNode failed", fv->vol); db->nfvols--; FreeMem(fv, sizeof *fv); continue; }
        add_vol(db, fv->vol, fv->dev);
        hdf_log(io, "folder %s: volume %s (%s:), not bootable%s", fv->path, fv->vol, fv->dev, db->readonly ? ", read-only" : "");
    }
out:
    FreeMem(paths, FOLDER_MAX * (128 + 32));
}
