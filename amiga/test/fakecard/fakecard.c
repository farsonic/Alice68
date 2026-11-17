/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A stand-in for alice68hdf.device's card service (hdf/cardsvc.h) on an
 * ordinary Amiga or emulator: put it in DEVS: as alice68hdf.device and assign
 * FAKECARD: to a folder laid out as Alice68's FAT partition (cmdline.txt,
 * config.txt, kernel8.img, HDF/, WIFI/, ...). Alice68Prefs and `Alice68 CARD`
 * then work on that folder, so their card pages can be tried without a Pi.
 * Only CS_FAT; reads and writes through dos.library in the caller's process
 * (the card service also runs in its caller's process). Test tool, not shipped.
 */
#include "../../amiga.h"
#include "../../hdf/cardsvc.h"

#define DEVNAME  "alice68hdf.device"
#define IDSTRING "alice68hdf.device 0.1 (fake card service for tests)"
ULONG lvo_x(APTR base, LONG off, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG a0, ULONG a1);

struct IOStdReq { struct IORequest io; ULONG io_Actual, io_Length; APTR io_Data; ULONG io_Offset; };
struct DevBase { struct Library lib; APTR seglist; APTR dos; struct Unit unit; };
struct FIB { LONG key, type; char name[108]; LONG prot, etype, size, blocks; LONG days, mins, ticks; char comment[80]; UWORD own[2]; char pad[32]; };

#define D(o, a, b, c) lvo_x(db->dos, o, 0, (ULONG)(a), (ULONG)(b), (ULONG)(c), 0, 0)
static void *allocm(ULONG n) { return (void *)lvo(SYSBASE, -198, n, 0x10000 /* MEMF_CLEAR */, 0, 0); }

static void path_of(char *out, const char *p)
{
    const char *pre = "FAKECARD:";
    int k = 0;
    while (*pre) out[k++] = *pre++;
    while (*p && k < CS_PATH + 10) out[k++] = *p++;
    out[k] = 0;
}

static int err_of(struct DevBase *db)
{
    LONG e = (LONG)D(-132, 0, 0, 0);
    return e == 205 || e == 204 ? CSE_NOTFOUND : e == 203 ? CSE_EXISTS : e == 216 ? CSE_NOTEMPTY :
           e == 214 || e == 223 ? CSE_RO : e == 202 ? CSE_INUSE : CSE_IO;
}

static void cpy(char *d, const char *s, int n) { while (--n > 0 && *s) *d++ = *s++; *d = 0; }

static int info(struct DevBase *db, struct cs_info *in)
{
    (void)db;
    char *p = (char *)in;
    for (ULONG i = 0; i < sizeof *in; i++) p[i] = 0;
    in->version = CS_VERSION;
    in->cmd64 = 1;
    in->nparts = 2;
    in->part[0].type = 0x0c; in->part[0].kind = CSP_FAT32; in->part[0].boot = 1;
    in->part[0].start = 8192; in->part[0].count = 524288; in->part[0].vol = CS_FAT;
    in->part[0].total_kb = 262144; in->part[0].free_kb = 131072;
    cpy(in->part[0].label, "BOOT", 16);
    in->part[1].type = 0x76; in->part[1].kind = CSP_AMIGA; in->part[1].start = 532480; in->part[1].count = 4194304;
    in->part[1].vol = ~0u; in->part[1].free_kb = ~0u;
    cpy(in->exfat_why, "no exFAT partition (fake card)", 80);
    cpy(in->card_dev, "fakecard (FAKECARD:)", 32);
    in->nused = 1; in->used[0].vol = CS_FAT; in->used[0].kind = 1; cpy(in->used[0].name, "HDF/Workbench.hdf", 64);
    return 0;
}

static int list(struct DevBase *db, struct cs_list *l, ULONG len)
{
    char p[CS_PATH + 16];
    struct FIB *f;
    ULONG lock, max = l->max;
    int e = 0;
    if (l->vol != CS_FAT) return CSE_NOVOL;
    if (len < sizeof *l || (len - (sizeof *l - sizeof l->ent)) / sizeof l->ent[0] < max)
        max = (len - (sizeof *l - sizeof l->ent)) / sizeof l->ent[0];
    path_of(p, l->path);
    l->count = l->more = 0;
    lock = D(-84, p, -2, 0);
    if (!lock) return err_of(db);
    f = allocm(sizeof *f);
    if (!f) { D(-90, lock, 0, 0); return IOERR_NOCMD; }
    if (!D(-102, lock, f, 0)) e = err_of(db);
    else if (f->type < 0) e = CSE_NOTDIR;
    else while (D(-108, lock, f, 0)) {
        struct cs_ent *x;
        if (l->count >= max) { l->more = 1; break; }
        x = &l->ent[l->count++];
        cpy(x->name, f->name, 108);
        x->size = f->type > 0 ? 0 : (ULONG)f->size; x->sizehi = 0;
        x->date = (0x5a21u << 16) | 0x6000u;
        x->flags = (f->type > 0 ? CSF_DIR : 0) | (f->prot & 4 ? CSF_RO : 0);
    }
    FreeMem(f, sizeof *f);
    D(-90, lock, 0, 0);
    return e;
}

static int readf(struct DevBase *db, struct cs_file *r, ULONG len)
{
    char p[CS_PATH + 16];
    ULONG fh, room = len > sizeof *r - 4 ? len - (sizeof *r - 4) : 0;
    LONG n, size;
    if (r->vol != CS_FAT) return CSE_NOVOL;
    if (r->max < room) room = r->max;
    path_of(p, r->path);
    fh = D(-30, p, 1005, 0);
    if (!fh) return err_of(db);
    D(-66, fh, 0, 1);                          /* Seek to the end */
    size = (LONG)D(-66, fh, 0, -1);            /* back: the old position = the size */
    r->len = (ULONG)size;
    if ((ULONG)size > room) { D(-36, fh, 0, 0); return CSE_TOOBIG; }
    n = (LONG)D(-42, fh, r->data, size);
    D(-36, fh, 0, 0);
    return n == size ? 0 : CSE_IO;
}

static int writef(struct DevBase *db, struct cs_file *r)
{
    char p[CS_PATH + 16];
    ULONG fh;
    LONG n;
    if (r->vol != CS_FAT) return CSE_NOVOL;
    path_of(p, r->path);
    fh = D(-30, p, 1006, 0);
    if (!fh) return err_of(db);
    n = (LONG)D(-48, fh, r->data, r->len);
    D(-36, fh, 0, 0);
    return n == (LONG)r->len ? 0 : CSE_IO;
}

static int simple(struct DevBase *db, UWORD cmd, struct cs_file *r)
{
    char p[CS_PATH + 16], q[CS_PATH + 16];
    ULONG ok;
    if (r->vol != CS_FAT) return CSE_NOVOL;
    path_of(p, r->path);
    if (cmd == CS_DELETE) ok = D(-72, p, 0, 0);
    else if (cmd == CS_MKDIR) { ok = D(-120, p, 0, 0); if (ok) D(-90, ok, 0, 0); }
    else { path_of(q, r->path2); ok = D(-78, p, q, 0); }
    return ok ? 0 : err_of(db);
}

LONG dev_open(struct DevBase *db, struct IOStdReq *io, ULONG unit, ULONG flags)
{
    (void)flags;
    io->io.io_Error = IOERR_OPENFAIL;
    if (unit != CS_UNIT) return -1;
    if (!db->dos) db->dos = OpenLibrary("dos.library", 37);
    if (!db->dos) return -1;
    io->io.io_Unit = &db->unit;
    io->io.io_Error = 0;
    db->unit.unit_OpenCnt++;
    db->lib.lib_OpenCnt++;
    db->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return 0;
}

static ULONG do_expunge(struct DevBase *db)
{
    ULONG seg = (ULONG)db->seglist;
    if (db->lib.lib_OpenCnt) { db->lib.lib_Flags |= LIBF_DELEXP; return 0; }
    if (db->dos) CloseLibrary(db->dos);
    Remove(&db->lib.lib_Node);
    FreeMem((UBYTE *)db - db->lib.lib_NegSize, (ULONG)db->lib.lib_NegSize + db->lib.lib_PosSize);
    return seg;
}

ULONG dev_close(struct DevBase *db, struct IOStdReq *io)
{
    io->io.io_Unit = (struct Unit *)-1;
    io->io.io_Device = (APTR)-1;
    if (db->unit.unit_OpenCnt) db->unit.unit_OpenCnt--;
    if (db->lib.lib_OpenCnt) db->lib.lib_OpenCnt--;
    if (!db->lib.lib_OpenCnt && (db->lib.lib_Flags & LIBF_DELEXP)) return do_expunge(db);
    return 0;
}

ULONG dev_expunge(struct DevBase *db) { return do_expunge(db); }

void dev_beginio(struct DevBase *db, struct IOStdReq *io)
{
    int e;
    io->io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    switch (io->io.io_Command) {
    case CS_INFO:   e = info(db, io->io_Data); break;
    case CS_LIST:   e = list(db, io->io_Data, io->io_Length); break;
    case CS_READ:   e = readf(db, io->io_Data, io->io_Length); io->io_Actual = e ? 0 : ((struct cs_file *)io->io_Data)->len; break;
    case CS_WRITE:  e = writef(db, io->io_Data); break;
    case CS_DELETE: case CS_RENAME: case CS_MKDIR: e = simple(db, io->io.io_Command, io->io_Data); break;
    default:        e = IOERR_NOCMD; break;
    }
    io->io.io_Error = (BYTE)e;
    if (!(io->io.io_Flags & IOF_QUICK)) ReplyMsg(io);
}

LONG dev_abortio(struct DevBase *db, struct IOStdReq *io) { (void)db; (void)io; return 0; }
void dev_irq(void *p) { (void)p; }

ULONG dev_init(struct DevBase *db, APTR seglist)
{
    db->seglist = seglist;
    db->lib.lib_Node.ln_Type = NT_DEVICE;
    db->lib.lib_Node.ln_Name = (char *)DEVNAME;
    db->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    db->lib.lib_Version = 0;
    db->lib.lib_Revision = 1;
    db->lib.lib_IdString = (APTR)IDSTRING;
    db->unit.unit_MsgPort.mp_Node.ln_Type = 4;
    db->unit.unit_MsgPort.mp_Flags = 2;
    return (ULONG)db;
}

const char dev_name[] = DEVNAME;
const char dev_idstring[] = IDSTRING;
extern void stub_init(void), stub_open(void), stub_close(void), stub_expunge(void),
            stub_null(void), stub_beginio(void), stub_abortio(void);
const ULONG dev_functab[] = {
    (ULONG)stub_open, (ULONG)stub_close, (ULONG)stub_expunge, (ULONG)stub_null,
    (ULONG)stub_beginio, (ULONG)stub_abortio, (ULONG)-1
};
const ULONG dev_inittab[4] = { sizeof(struct DevBase), (ULONG)dev_functab, 0, (ULONG)stub_init };
