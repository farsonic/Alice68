/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice68hdf.device: CD images (*.iso) on the SD card's FAT partition as
 * read-only Amiga volumes.
 *
 * Every *.iso in the partition's root and in a root folder named ISO is
 * mounted (up to ISO_MAX; cmdline.txt iso=off turns them off), as DOS
 * devices CD0:, CD1:, ... (a name already mounted is skipped), with the
 * disc's own label as the volume name. Never bootable. ISO 9660 with Joliet
 * and Rock Ridge names (iso9660.c); no CD filesystem has to be installed.
 *
 * Like the folders (folders.c), each is a DOS handler process started by
 * dos.library at boot, its code here in the ROM. Each process reads the card
 * itself; nothing is shared between them. A mounted image is read-only
 * through a folder too.
 */
#include "hdf_dev.h"
#include "dosdefs.h"
#include "iso9660.h"

#define ISO_STACK  8192
extern void iso_main(struct HdfBase *db);

struct IVol {                               /* starts as struct IVolHead (hdf_dev.h) */
    struct IVolHead h;
    struct iso iso;                         /* opened by the handler */
    struct DosList *dl;
};
ACHECK(AOFF(struct IVol, h) == 0);

/* a lock or an open file */
struct IObj {
    struct FileLock fl;                     /* first: a lock's BPTR points here */
    struct IObj *next;
    ULONG isfile;
    struct iso_ent e;
    ULONG pos;
};

/* one handler process */
struct ICtx {
    struct HdfBase *db;
    struct IVol *iv;
    struct card c;
    struct MsgPort *cport;
    struct hdf_io io;
    APTR dos;
    struct MsgPort *port;
    struct IObj *objs;
};

static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, uint32_t max) { uint32_t k = 0; for (; s[k] && k + 1 < max; k++) d[k] = s[k]; d[k] = 0; }

static LONG dos_err(int r)
{
    switch (r) {
    case ISO_END:     return ERROR_OBJECT_NOT_FOUND;
    case ISO_NOTDIR:  return ERROR_OBJECT_WRONG_TYPE;
    case ISO_CORRUPT: case ISO_NOTISO: return ERROR_NOT_A_DOS_DISK;
    default:          return ERROR_SEEK_ERROR;
    }
}

/* ---- objects ------------------------------------------------------------------------ */
static int is_root(const struct ICtx *x, const struct iso_ent *e) { return e->isdir && e->extent == x->iv->iso.root.extent; }
static int same(const struct iso_ent *a, const struct iso_ent *b)
{
    if (a->isdir != b->isdir) return 0;
    return a->isdir ? a->extent == b->extent : (a->dir == b->dir && a->pos == b->pos);
}

static struct IObj *new_obj(struct ICtx *x, const struct iso_ent *e, LONG access, int isfile)
{
    struct IObj *o = AllocMem(sizeof *o, MEMF_PUBLIC | MEMF_CLEAR);
    if (!o) return 0;
    o->e = *e;
    o->isfile = (ULONG)isfile;
    o->fl.fl_Key = (LONG)(e->isdir ? e->extent : e->extent ^ (e->pos << 20));
    o->fl.fl_Access = access == EXCLUSIVE_LOCK ? EXCLUSIVE_LOCK : SHARED_LOCK;
    o->fl.fl_Task = x->port;
    o->fl.fl_Volume = MKBADDR(x->iv->dl);
    o->next = x->objs;
    x->objs = o;
    return o;
}
static void free_obj(struct ICtx *x, struct IObj *o)
{
    for (struct IObj **p = &x->objs; *p; p = &(*p)->next)
        if (*p == o) { *p = o->next; break; }
    FreeMem(o, sizeof *o);
}
static struct IObj *lock_obj(struct ICtx *x, LONG bptr)
{
    struct IObj *o = BADDR(bptr);
    for (struct IObj *p = x->objs; p; p = p->next) if (p == o) return o;
    return 0;
}

/* a BSTR path from lock: 0 found (*e), <0 -error */
static int resolve(struct ICtx *x, LONG lock, ULONG bname, struct iso_ent *e)
{
    struct iso *v = &x->iv->iso;
    if (!lock) *e = v->root;
    else {
        struct IObj *o = lock_obj(x, lock);
        if (!o) return -ERROR_INVALID_LOCK;
        *e = o->e;
    }
    const UBYTE *b = BADDR(bname);
    uint32_t len = b ? b[0] : 0;
    const char *s = (const char *)b + 1;
    for (uint32_t i = 0; i < len; i++) if (s[i] == ':') { s += i + 1; len -= i + 1; i = ~0u; }   /* "VOL:", "CD0:" */
    uint32_t i = 0;
    while (i < len) {
        int r;
        if (s[i] == '/') {
            if (is_root(x, e)) return -ERROR_OBJECT_NOT_FOUND;
            struct iso_ent p;
            if ((r = iso_parent(v, e, &p))) return -dos_err(r);
            *e = p;
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < len && s[j] != '/') j++;
        if (!e->isdir) return -ERROR_OBJECT_WRONG_TYPE;
        struct iso_ent n;
        r = iso_find(v, e, s + i, j - i, &n);
        if (r == ISO_END) return j < len ? -ERROR_DIR_NOT_FOUND : -ERROR_OBJECT_NOT_FOUND;
        if (r) return -dos_err(r);
        *e = n;
        i = j < len ? j + 1 : j;
    }
    return 0;
}

/* ---- packets ---------------------------------------------------------------------------- */
static void fill_fib(struct ICtx *x, struct FileInfoBlock *fib, const struct iso_ent *e)
{
    memset(fib, 0, sizeof *fib);
    int root = is_root(x, e);
    const char *n = root ? x->iv->h.vol : e->name;
    uint32_t k = slen(n);
    if (k > 106) k = 106;
    fib->fib_FileName[0] = (char)k;
    memcpy(fib->fib_FileName + 1, n, k);
    fib->fib_DirEntryType = fib->fib_EntryType = root ? ST_ROOT : e->isdir ? ST_USERDIR : ST_FILE;
    fib->fib_Size = e->isdir ? 0 : (LONG)e->size;
    fib->fib_NumBlocks = (LONG)((e->size + ISO_SEC - 1) / ISO_SEC);
    fib->fib_Protection = FIBF_WRITE | FIBF_DELETE;           /* read-only: no writing, no deleting */
    uint32_t d, m, t;
    iso_date(e->date, &d, &m, &t);
    fib->fib_Date.ds_Days = (LONG)d; fib->fib_Date.ds_Minute = (LONG)m; fib->fib_Date.ds_Tick = (LONG)t;
}

static void fill_info(struct ICtx *x, struct InfoData *id)
{
    memset(id, 0, sizeof *id);
    id->id_UnitNumber = (LONG)x->iv->h.index;
    id->id_DiskState = ID_WRITE_PROTECTED;
    id->id_NumBlocks = (LONG)x->iv->iso.blocks;
    id->id_NumBlocksUsed = (LONG)x->iv->iso.blocks;
    id->id_BytesPerBlock = ISO_SEC;
    id->id_DiskType = ID_DOS_DISK;
    id->id_VolumeNode = MKBADDR(x->iv->dl);
    id->id_InUse = x->objs ? DOSTRUE : DOSFALSE;
}

/* returns Res1; *res2 the error */
static LONG do_packet(struct ICtx *x, struct DosPacket *p, LONG *res2)
{
    struct iso *v = &x->iv->iso;
    struct iso_ent e;
    struct IObj *o;
    int r;
    *res2 = 0;
#define FAIL(err) do { *res2 = (err); return DOSFALSE; } while (0)
#define RESOLVE(lock, name) do { r = resolve(x, (lock), (ULONG)(name), &e); if (r < 0) FAIL(-r); } while (0)
    switch (p->dp_Type) {
    case ACTION_LOCATE_OBJECT:
        RESOLVE(p->dp_Arg1, p->dp_Arg2);
        if (!(o = new_obj(x, &e, p->dp_Arg3, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_COPY_DIR:
    case ACTION_COPY_DIR_FH:
        if (p->dp_Type == ACTION_COPY_DIR_FH) e = ((struct IObj *)p->dp_Arg1)->e;
        else if (!p->dp_Arg1) e = v->root;
        else { if (!(o = lock_obj(x, p->dp_Arg1))) FAIL(ERROR_INVALID_LOCK); e = o->e; }
        if (!(o = new_obj(x, &e, SHARED_LOCK, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);

    case ACTION_FREE_LOCK:
        if (p->dp_Arg1) { if (!(o = lock_obj(x, p->dp_Arg1))) FAIL(ERROR_INVALID_LOCK); free_obj(x, o); }
        return DOSTRUE;

    case ACTION_PARENT:
    case ACTION_PARENT_FH: {
        if (p->dp_Type == ACTION_PARENT_FH) e = ((struct IObj *)p->dp_Arg1)->e;
        else {
            if (!p->dp_Arg1) return 0;
            if (!(o = lock_obj(x, p->dp_Arg1))) FAIL(ERROR_INVALID_LOCK);
            e = o->e;
        }
        if (is_root(x, &e)) return 0;                     /* the root has none: 0, no error */
        struct iso_ent par;
        if ((r = iso_parent(v, &e, &par))) FAIL(dos_err(r));
        if (!(o = new_obj(x, &par, SHARED_LOCK, 0))) FAIL(ERROR_NO_FREE_STORE);
        return (LONG)MKBADDR(o);
    }

    case ACTION_SAME_LOCK: {
        struct IObj *a = lock_obj(x, p->dp_Arg1), *b = lock_obj(x, p->dp_Arg2);
        if (!a || !b) FAIL(ERROR_INVALID_LOCK);
        return same(&a->e, &b->e) ? DOSTRUE : DOSFALSE;
    }

    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_FH:
        if (p->dp_Type == ACTION_EXAMINE_FH) e = ((struct IObj *)p->dp_Arg1)->e;
        else if (!p->dp_Arg1) e = v->root;
        else { if (!(o = lock_obj(x, p->dp_Arg1))) FAIL(ERROR_INVALID_LOCK); e = o->e; }
        fill_fib(x, BADDR(p->dp_Arg2), &e);
        return DOSTRUE;

    case ACTION_EXAMINE_NEXT: {
        struct FileInfoBlock *fib = BADDR(p->dp_Arg2);
        if (!p->dp_Arg1) e = v->root;
        else { if (!(o = lock_obj(x, p->dp_Arg1))) FAIL(ERROR_INVALID_LOCK); e = o->e; }
        if (!e.isdir) FAIL(ERROR_OBJECT_WRONG_TYPE);
        uint32_t pos = (uint32_t)fib->fib_DiskKey;
        struct iso_ent n;
        r = iso_next(v, &e, &pos, &n);
        if (r == ISO_END) FAIL(ERROR_NO_MORE_ENTRIES);
        if (r) FAIL(dos_err(r));
        fill_fib(x, fib, &n);
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

    case ACTION_FINDINPUT: {
        struct FileHandle *fh = BADDR(p->dp_Arg1);
        RESOLVE(p->dp_Arg2, p->dp_Arg3);
        if (e.isdir) FAIL(ERROR_OBJECT_WRONG_TYPE);
        if (!(o = new_obj(x, &e, SHARED_LOCK, 1))) FAIL(ERROR_NO_FREE_STORE);
        fh->fh_Arg1 = (LONG)o;
        fh->fh_Port = 0;                               /* not interactive */
        return DOSTRUE;
    }

    case ACTION_FH_FROM_LOCK: {
        struct FileHandle *fh = BADDR(p->dp_Arg1);
        if (!(o = lock_obj(x, p->dp_Arg2))) FAIL(ERROR_INVALID_LOCK);
        if (o->e.isdir) FAIL(ERROR_OBJECT_WRONG_TYPE);
        o->isfile = 1; o->pos = 0;
        fh->fh_Arg1 = (LONG)o;
        fh->fh_Port = 0;
        return DOSTRUE;
    }

    case ACTION_END:
        free_obj(x, (struct IObj *)p->dp_Arg1);
        return DOSTRUE;

    case ACTION_READ: {
        o = (struct IObj *)p->dp_Arg1;
        int32_t n = iso_read(v, &o->e, o->pos, (APTR)p->dp_Arg2, (ULONG)p->dp_Arg3);
        if (n < 0) { *res2 = dos_err(n); return -1; }
        o->pos += (ULONG)n;
        return n;
    }

    case ACTION_SEEK: {
        o = (struct IObj *)p->dp_Arg1;
        LONG base = p->dp_Arg3 == OFFSET_BEGINNING ? 0 : p->dp_Arg3 == OFFSET_END ? (LONG)o->e.size : (LONG)o->pos;
        LONG np = base + p->dp_Arg2;
        if ((p->dp_Arg3 < -1 || p->dp_Arg3 > 1) || np < 0 || (ULONG)np > o->e.size) { *res2 = ERROR_SEEK_ERROR; return -1; }
        LONG old = (LONG)o->pos;
        o->pos = (ULONG)np;
        return old;
    }

    case ACTION_WRITE:
    case ACTION_SET_FILE_SIZE:
        *res2 = ERROR_DISK_WRITE_PROTECTED;
        return -1;

    case ACTION_FINDOUTPUT:                            /* a CD image is never written */
    case ACTION_FINDUPDATE:
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT:
    case ACTION_CREATE_DIR:
    case ACTION_SET_PROTECT:
    case ACTION_SET_DATE:
    case ACTION_SET_COMMENT:
    case ACTION_SET_OWNER:
    case ACTION_RENAME_DISK:
        FAIL(ERROR_DISK_WRITE_PROTECTED);

    case ACTION_CHANGE_MODE: {
        o = p->dp_Arg1 == 0 ? lock_obj(x, p->dp_Arg2) : (struct IObj *)((struct FileHandle *)BADDR(p->dp_Arg2))->fh_Arg1;
        if (!o) FAIL(ERROR_INVALID_LOCK);
        o->fl.fl_Access = (p->dp_Arg3 == EXCLUSIVE_LOCK || p->dp_Arg3 == MODE_NEWFILE) ? EXCLUSIVE_LOCK : SHARED_LOCK;
        return DOSTRUE;
    }

    case ACTION_CURRENT_VOLUME:
        return (LONG)MKBADDR(x->iv->dl);
    case ACTION_IS_FILESYSTEM:
    case ACTION_FLUSH:
    case ACTION_INHIBIT:
        return DOSTRUE;
    default:
        FAIL(ERROR_ACTION_NOT_KNOWN);
    }
#undef FAIL
#undef RESOLVE
}

/* one packet; for iso_main and the 68k test harness */
LONG iso_do(struct ICtx *x, struct DosPacket *p)
{
    LONG res2;
    LONG res1 = do_packet(x, p, &res2);
    p->dp_Res1 = res1;
    p->dp_Res2 = res2;
    return res1;
}

/* the handler's start: card, the image, dos.library, the volume node. 0 = cannot run */
struct ICtx *iso_start(struct HdfBase *db, ULONG unit, struct MsgPort *port)
{
    if (unit < ISO_UNIT0 || unit - ISO_UNIT0 >= db->nivols) return 0;
    struct IVol *iv = db->ivol[unit - ISO_UNIT0];
    struct ICtx *x = AllocMem(sizeof *x, MEMF_PUBLIC | MEMF_CLEAR);
    if (!x) return 0;
    x->db = db; x->iv = iv; x->port = port;
    struct hdf_io io = HDF_IO(&x->c);
    x->io = io;
    x->dos = OpenLibrary("dos.library", 36);
    if (!x->dos || card_open(db, &x->c, &x->cport)) {
        hdf_log(&x->io, "%s: cannot start (dos.library or %s)", iv->h.vol, db->card_dev);
        if (x->dos) CloseLibrary(x->dos);
        FreeMem(x, sizeof *x);
        return 0;
    }
    if (iso_open(&iv->iso, &x->io, &iv->h.f)) {
        hdf_log(&x->io, "%s: the CD image cannot be read", iv->h.path);
        card_close(&x->c, x->cport);
        CloseLibrary(x->dos);
        FreeMem(x, sizeof *x);
        return 0;
    }
    uint32_t n = slen(iv->h.vol);
    struct DosList *dl = AllocMem(sizeof *dl + 36, MEMF_PUBLIC | MEMF_CLEAR);
    if (dl) {
        UBYTE *name = (UBYTE *)(dl + 1);               /* longword-aligned BSTR after it */
        name[0] = (UBYTE)n;
        memcpy(name + 1, iv->h.vol, n);
        dl->dol_Type = DLT_VOLUME;
        dl->dol_Task = port;
        dl->dol_DiskType = ID_DOS_DISK;
        dl->dol_Name = MKBADDR(name);
        uint32_t d, m, t;
        iso_date(iv->iso.root.date, &d, &m, &t);
        dl->dol_VolumeDate.ds_Days = (LONG)d; dl->dol_VolumeDate.ds_Minute = (LONG)m; dl->dol_VolumeDate.ds_Tick = (LONG)t;
        LockDosList(x->dos, LDF_VOLUMES | LDF_WRITE);
        if (!AddDosEntry(x->dos, dl)) { FreeMem(dl, sizeof *dl + 36); dl = 0; }
        UnLockDosList(x->dos, LDF_VOLUMES | LDF_WRITE);
    }
    iv->dl = dl;
    if (!dl) hdf_log(&x->io, "%s: the volume could not be added", iv->h.vol);
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

/* a handler process (called from the RAM seglist isos_mount() made) */
void iso_main(struct HdfBase *db)
{
    struct MsgPort *port = (struct MsgPort *)((UBYTE *)FindTask(0) + 92);   /* pr_MsgPort */
    WaitPort(port);
    struct Message *m = GetMsg(port);
    struct DosPacket *p = (struct DosPacket *)m->mn_Node.ln_Name;
    struct FileSysStartupMsg *fssm = BADDR(p->dp_Arg2);
    struct DeviceNode *dn = BADDR(p->dp_Arg3);
    struct ICtx *x = fssm ? iso_start(db, fssm->fssm_Unit, port) : 0;
    if (!x) { p->dp_Res1 = DOSFALSE; p->dp_Res2 = ERROR_NO_FREE_STORE; reply_pkt(p, port); return; }
    dn->dn_Task = (ULONG)port;
    p->dp_Res1 = DOSTRUE; p->dp_Res2 = 0;
    reply_pkt(p, port);
    for (;;) {
        WaitPort(port);
        while ((m = GetMsg(port))) {
            p = (struct DosPacket *)m->mn_Node.ln_Name;
            iso_do(x, p);
            reply_pkt(p, port);
        }
    }
}

/* ---- at boot: the images, their volume names and boot nodes ------------------------------- */
static int name_taken(struct HdfBase *db, const char *vol)
{
    if (find_vol(db, vol) >= 0) return 1;
    for (ULONG k = 0; k < db->nfvols; k++) if (str_ieq(((struct FVolHead *)db->fvol[k])->vol, vol)) return 1;
    for (ULONG k = 0; k < db->nivols; k++) if (str_ieq(db->ivol[k]->h.vol, vol)) return 1;
    return 0;
}

void isos_mount(struct HdfBase *db, struct hdf_io *io, APTR eb, APTR cd, struct hdf_file *files, int n)
{
    for (int i = 0; i < n; i++) {
        struct hdf_file *h = &files[i];
        if (!h->ext) continue;
        if (db->nivols >= ISO_MAX) { hdf_file_free(io, h); continue; }
        uint32_t top = 0;
        for (uint32_t k = 0; k < h->next; k++)
            if (h->ext[k].card_sec + h->ext[k].count > top) top = h->ext[k].card_sec + h->ext[k].count;
        if (!db->cmd64 && top > 0x800000u) {
            hdf_log(io, "%s: lies beyond the card's first 4 GB, which %s cannot reach", h->name, db->card_dev);
            hdf_file_free(io, h);
            continue;
        }
        struct iso t;
        int r = iso_open(&t, io, h);
        if (r) {
            hdf_log(io, "%s: %s - ignored", h->name, r == ISO_NOTISO ? "not an ISO 9660 CD image" : "unreadable");
            hdf_file_free(io, h);
            continue;
        }
        struct IVol *iv = AllocMem(sizeof *iv, MEMF_PUBLIC | MEMF_CLEAR);
        if (!iv) { iso_close(&t); hdf_file_free(io, h); break; }
        iv->h.index = db->nivols;
        scpy(iv->h.path, h->name, sizeof iv->h.path);
        iv->h.f = *h;
        h->ext = 0;                                    /* the volume owns the extents now */
        /* the volume name: the disc's label, else the file's name; never one mounted already */
        char base[32];
        scpy(base, t.vol, 28);
        const char *kind = t.rr ? "Rock Ridge" : t.joliet ? "Joliet" : "ISO 9660";
        uint32_t blocks = t.blocks;
        iso_close(&t);
        if (!base[0]) {
            scpy(base, iv->h.path, 28);
            uint32_t l = slen(base);
            if (l > 4 && base[l - 4] == '.') base[l - 4] = 0;
        }
        scpy(iv->h.vol, base, 28);
        for (char d = '2'; d <= '9' && name_taken(db, iv->h.vol); d++) {
            uint32_t l = slen(base) < 27 ? slen(base) : 27;
            memcpy(iv->h.vol, base, l); iv->h.vol[l] = d; iv->h.vol[l + 1] = 0;
        }
        /* the DOS device: CD0, CD1, ... not one mounted already */
        scpy(iv->h.dev, "CD0", sizeof iv->h.dev);
        for (char d = '0'; d <= '9'; d++) {
            iv->h.dev[2] = d;
            if (!find_drive(eb, iv->h.dev)) break;
        }
        if (!db->iseg && !(db->iseg = handler_seglist(db, iso_main))) { hdf_file_free(io, &iv->h.f); FreeMem(iv, sizeof *iv); break; }
        ULONG pkt[4 + 17] = { (ULONG)iv->h.dev, (ULONG)hdf_devname, ISO_UNIT0 + iv->h.index, 0,
                              16, ISO_SEC / 4, 0, 1, 1, 1, 0, 0, 0, 0, blocks ? blocks - 1 : 0, 1, 1, 0x1fe00, 0x7ffffffe,
                              (ULONG)-128, 0x43443031 /* 'CD01' */ };
        struct DeviceNode *dn = eb ? MakeDosNode(eb, pkt) : 0;
        if (!dn) { hdf_log(io, "%s: MakeDosNode failed", iv->h.path); hdf_file_free(io, &iv->h.f); FreeMem(iv, sizeof *iv); continue; }
        dn->dn_SegList = MKBADDR(db->iseg);
        dn->dn_GlobalVec = (ULONG)-1;                   /* a C handler */
        dn->dn_StackSize = ISO_STACK;
        dn->dn_Priority = 10;
        dn->dn_Handler = 0;
        db->ivol[db->nivols++] = iv;
        if (!AddBootNode(eb, -128, 1 /* ADNF_STARTPROC */, dn, cd)) {
            hdf_log(io, "%s: AddBootNode failed", iv->h.path);
            db->nivols--;
            hdf_file_free(io, &iv->h.f);
            FreeMem(iv, sizeof *iv);
            continue;
        }
        add_vol(db, iv->h.vol, iv->h.dev);
        hdf_log(io, "CD image %s: volume %s (%s:), %s, %lu MB, read-only", iv->h.path, iv->h.vol, iv->h.dev, kind,
                (blocks * (ISO_SEC / 512)) >> 11);
    }
}
