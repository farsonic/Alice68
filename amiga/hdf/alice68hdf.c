/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice68hdf.device - Amiga hardfiles (*.hdf) on the Pi's SD card.
 *
 * At start (from Emu68's ROM board, after brcm-emmc.device) it opens the
 * whole card (brcm-emmc.device unit 0, `dtoverlay=emmc,unit0=ro` or `rw`),
 * finds the FAT boot partition and every *.hdf in its root directory and in
 * a root directory named HDF, works out where each file's blocks are on the
 * card, and adds each hardfile's partitions to the boot list: those of its
 * Rigid Disk Block, or for a single-partition hardfile (a filesystem from
 * block 0, as WinUAE makes them) one partition, DH<n>. Each hardfile is a unit.
 *
 * cmdline.txt (read from the FAT partition): hdf=off, hdf.readonly, and
 * hdf.boot=NAME - that hardfile boots, and where one of its partitions has
 * the name of a drive already mounted (an SD card partition, say DH0) it
 * takes the name over and the other drive is not mounted. Without it, the
 * drive mounted first keeps the name and the hardfile's partition is left
 * out: a drive is never there twice.
 *
 * Writes go to the file's own clusters on the card, nowhere else; the FAT
 * itself is never changed. Partitions whose filesystem is not FFS and not
 * already in FileSystem.resource are left out.
 * One task does the card I/O, in arrival order; the filesystems' buffers are
 * handed straight to brcm-emmc.device (no copy).
 *
 * Everything writable lives in the device base: the code runs from the ROM
 * board, which is mapped read-only.
 */
#include "../amiga.h"
#include "hdf_core.h"

#define VERSION   0
#define REVISION  2
const char hdf_devname[] = "alice68hdf.device";
const char hdf_idstring[] = "alice68hdf.device 0.2 (05.11.2026) Alice68 hardfiles";

#define HDF_MAGIC  0x41363848u              /* 'A68H', read by Alice68-HDF */
#define LOG_SIZE   4096
#define TASK_STACK 8192
#define MAX_CHUNK  256u                     /* sectors per card request */

#define MEMF_PUBLIC 1
#define MEMF_CLEAR  0x10000
#define NT_TASK     1
#define NT_REPLYMSG 7
#define NT_MSGPORT  4
#define PA_SIGNAL   0

#define CMD_READ        2
#define CMD_WRITE       3
#define CMD_UPDATE      4
#define CMD_CLEAR       5
#define TD_MOTOR        9
#define TD_FORMAT       11
#define TD_REMOVE       12
#define TD_CHANGENUM    13
#define TD_CHANGESTATE  14
#define TD_PROTSTATUS   15
#define TD_GETDRIVETYPE 18
#define TD_GETNUMTRACKS 19
#define TD_ADDCHANGEINT 20
#define TD_REMCHANGEINT 21
#define TD_GETGEOMETRY  22
#define TD_EJECT        23
#define TD_READ64       24
#define TD_WRITE64      25
#define TD_FORMAT64     27
#define NSCMD_TD_READ64   0xc000
#define NSCMD_TD_WRITE64  0xc001
#define NSCMD_TD_FORMAT64 0xc002
#define TDERR_WriteProt   28
#define TDERR_NotSpecified 20
#define IOERR_BADLENGTH  (-4)
#define IOERR_BADADDRESS (-5)
#define DRIVE3_5         1

struct IOStdReq { struct IORequest req; ULONG io_Actual, io_Length; APTR io_Data; ULONG io_Offset; };
ACHECK(sizeof(struct IOStdReq) == 48);

struct Task {
    struct Node tc_Node; UBYTE tc_Flags, tc_State; BYTE tc_IDNestCnt, tc_TDNestCnt;
    ULONG tc_SigAlloc, tc_SigWait, tc_SigRecvd, tc_SigExcept;
    UWORD tc_TrapAlloc, tc_TrapAble; APTR tc_ExceptData, tc_ExceptCode, tc_TrapData, tc_TrapCode;
    APTR tc_SPReg, tc_SPLower, tc_SPUpper; APTR tc_Switch, tc_Launch;
    struct List tc_MemEntry; APTR tc_UserData;
};
ACHECK(AOFF(struct Task, tc_SPReg) == 54);
ACHECK(AOFF(struct Task, tc_UserData) == 88);

struct DriveGeometry { ULONG dg_SectorSize, dg_TotalSectors, dg_Cylinders, dg_CylSectors, dg_Heads, dg_TrackSectors, dg_BufMemType;
                       UBYTE dg_DeviceType, dg_Flags; UWORD dg_Reserved; };
struct NSDeviceQueryResult { ULONG DevQueryFormat, SizeAvailable; UWORD DeviceType, DeviceSubType; const UWORD *SupportedCommands; };

/* dos/filehandler.h DeviceNode, resources/filesysres.h FileSysEntry */
struct DeviceNode { ULONG dn_Next, dn_Type, dn_Task, dn_Lock, dn_Handler, dn_StackSize; LONG dn_Priority;
                    ULONG dn_Startup, dn_SegList, dn_GlobalVec, dn_Name; };
struct FileSysEntry { struct Node fse_Node; ULONG fse_DosType, fse_Version, fse_PatchFlags; ULONG fse_Type; /* ... */ };
struct FileSysResource { struct Node fsr_Node; char *fsr_Creator; struct List fsr_FileSysEntries; };
struct BootNode { struct Node bn_Node; UWORD bn_Flags; struct DeviceNode *bn_DeviceNode; };
ACHECK(AOFF(struct FileSysEntry, fse_Type) == 26);
ACHECK(AOFF(struct DeviceNode, dn_Name) == 40);
#define EB_MOUNTLIST 74                     /* struct ExpansionBase: MountList */

struct HUnit {
    struct Unit u;
    struct hdf_file f;                      /* Alice68-HDF reads up to here */
    struct hdf_rdb rdb;
    ULONG index;
    ULONG chosen;                           /* hdf.boot names it */
    ULONG single;                           /* no RDB: one partition from block 0 */
};

struct HdfBase {
    struct Library lib;
    UWORD pad;
    ULONG magic;                            /* 36: HDF_MAGIC */
    ULONG nunits;                           /* 40 */
    struct HUnit *unit[HDF_MAX_FILES];      /* 44 */
    ULONG log_wr;                           /* bytes ever written to log[] */
    char log[LOG_SIZE];                     /* Alice68-HDF reads up to here */
    const char *card_dev;                   /* brcm-emmc.device or brcm-sdhc.device */
    ULONG cmd64;                            /* 0: below 4 GB only, else the 64-bit read command */
    ULONG readonly;                         /* hdf.readonly, or the card's unit 0 is read-only */
    struct hdf_opts opts;
    struct MsgPort port;                    /* requests for the task */
    struct Task task;
    APTR stack;
    ULONG reads, read_errors, writes, write_errors;
};
ACHECK(AOFF(struct HdfBase, magic) == 36);
ACHECK(AOFF(struct HdfBase, unit) == 44);
ACHECK(AOFF(struct HdfBase, log) == 112);

/* the card, for hdf_core: one IORequest, used by one task at a time */
struct card { struct HdfBase *db; struct IOStdReq *io; };

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

static APTR AllocMem(ULONG n, ULONG req)              { return (APTR)lvo(SYSBASE, -198, n, req, 0, 0); }
static ULONG Wait(ULONG mask)                         { return lvo(SYSBASE, -318, mask, 0, 0, 0); }
static BYTE AllocSignal(LONG n)                       { return (BYTE)lvo(SYSBASE, -330, (ULONG)n, 0, 0, 0); }
static void PutMsg(struct MsgPort *p, APTR m)         { lvo(SYSBASE, -366, 0, 0, (ULONG)p, (ULONG)m); }
static APTR GetMsg(struct MsgPort *p)                 { return (APTR)lvo(SYSBASE, -372, 0, 0, (ULONG)p, 0); }
static BYTE DoIO(APTR io)                             { return (BYTE)lvo(SYSBASE, -456, 0, 0, 0, (ULONG)io); }
static APTR OpenResource(const char *n)               { return (APTR)lvo(SYSBASE, -498, 0, 0, 0, (ULONG)n); }
static struct MsgPort *CreateMsgPort(void)            { return (struct MsgPort *)lvo(SYSBASE, -666, 0, 0, 0, 0); }
static void DeleteMsgPort(struct MsgPort *p)          { lvo(SYSBASE, -672, 0, 0, (ULONG)p, 0); }
static APTR CreateIORequest(struct MsgPort *p, ULONG n) { return (APTR)lvo(SYSBASE, -654, n, 0, (ULONG)p, 0); }
static void DeleteIORequest(APTR io)                  { lvo(SYSBASE, -660, 0, 0, (ULONG)io, 0); }
static APTR MakeDosNode(APTR eb, ULONG *pkt)          { return (APTR)lvo(eb, -144, 0, 0, (ULONG)pkt, 0); }
static LONG AddBootNode(APTR eb, LONG pri, ULONG fl, APTR dn, APTR cd) { return (LONG)lvo(eb, -36, (ULONG)pri, fl, (ULONG)dn, (ULONG)cd); }
extern void add_task(struct Task *t, APTR initpc);
extern void task_entry(void);

static void NewList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = 0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

/* ---- log ------------------------------------------------------------------ */
static void log_line(void *ctx, const char *line)
{
    struct HdfBase *db = ((struct card *)ctx)->db;
    const char *p = "[alice68hdf] ";
    Forbid();
    for (int pass = 0; pass < 2; pass++, p = line)
        for (; *p; p++) db->log[db->log_wr++ % LOG_SIZE] = *p;
    db->log[db->log_wr++ % LOG_SIZE] = '\n';
    Permit();
}

/* ---- the card ---------------------------------------------------------------- */
/* one request of up to MAX_CHUNK sectors: 0 ok, else io_Error */
static int card_xfer(struct card *c, int write, uint32_t sector, uint32_t n, APTR p)
{
    struct IOStdReq *io = c->io;
    io->io_Data = p;
    io->io_Length = n * HDF_BLOCK;
    io->io_Offset = sector << 9;
    if (sector < 0x800000u && sector + n <= 0x800000u) {
        io->req.io_Command = write ? CMD_WRITE : CMD_READ;
        io->io_Actual = 0;
    } else {
        if (!c->db->cmd64) return IOERR_BADADDRESS;
        io->req.io_Command = (UWORD)(c->db->cmd64 + (write ? 1 : 0));   /* TD_WRITE64 / NSCMD_TD_WRITE64 follow the reads */
        io->io_Actual = sector >> 23;                   /* the high 32 bits of the byte offset */
    }
    BYTE e = DoIO(io);
    if (!e && io->io_Actual != n * HDF_BLOCK) e = TDERR_NotSpecified;
    return e;
}

static int card_read(void *ctx, uint32_t sector, uint32_t count, void *buf)
{
    struct card *c = ctx;
    UBYTE *p = buf;
    while (count) {
        uint32_t n = count > MAX_CHUNK ? MAX_CHUNK : count;
        c->db->reads++;
        if (card_xfer(c, 0, sector, n, p)) { c->db->read_errors++; return -1; }
        sector += n; count -= n; p += n * HDF_BLOCK;
    }
    return 0;
}

static int card_write(void *ctx, uint32_t sector, uint32_t count, const void *buf)
{
    struct card *c = ctx;
    const UBYTE *p = buf;
    while (count) {
        uint32_t n = count > MAX_CHUNK ? MAX_CHUNK : count;
        c->db->writes++;
        int e = card_xfer(c, 1, sector, n, (APTR)p);
        if (e) { c->db->write_errors++; return e == TDERR_WriteProt ? -2 : -1; }
        sector += n; count -= n; p += n * HDF_BLOCK;
    }
    return 0;
}
static void *core_alloc(void *ctx, uint32_t n) { (void)ctx; return AllocMem(n, MEMF_PUBLIC | MEMF_CLEAR); }
static void core_free(void *ctx, void *p, uint32_t n) { (void)ctx; FreeMem(p, n); }
#define HDF_IO(c) { (c), card_read, card_write, core_alloc, core_free, log_line }

static int card_open(struct HdfBase *db, struct card *c, struct MsgPort **port)
{
    *port = CreateMsgPort();
    if (!*port) return -1;
    c->db = db;
    c->io = CreateIORequest(*port, sizeof(struct IOStdReq));
    if (!c->io) { DeleteMsgPort(*port); return -1; }
    static const char *const names[] = { "brcm-emmc.device", "brcm-sdhc.device" };
    for (unsigned i = 0; i < 2; i++) {
        if (db->card_dev && db->card_dev != names[i]) continue;
        if (OpenDevice(names[i], 0, c->io, 0) == 0) { db->card_dev = names[i]; return 0; }
    }
    DeleteIORequest(c->io);
    DeleteMsgPort(*port);
    return -1;
}

static void card_close(struct card *c, struct MsgPort *port)
{
    CloseDevice(c->io);
    DeleteIORequest(c->io);
    DeleteMsgPort(port);
}

/* which 64-bit read command the card driver takes, if any */
static ULONG probe64(struct card *c, APTR buf)
{
    struct NSDeviceQueryResult q;
    memset(&q, 0, sizeof q);
    struct IOStdReq *io = c->io;
    io->req.io_Command = NSCMD_DEVICEQUERY;
    io->io_Data = &q; io->io_Length = sizeof q; io->io_Actual = 0; io->io_Offset = 0;
    if (DoIO(io) == 0 && io->io_Actual >= 16 && q.SupportedCommands)
        for (const UWORD *w = q.SupportedCommands; *w; w++)
            if (*w == NSCMD_TD_READ64) return NSCMD_TD_READ64;
    io->req.io_Command = TD_READ64;
    io->io_Data = buf; io->io_Length = HDF_BLOCK; io->io_Offset = 0; io->io_Actual = 0;
    if (DoIO(io) == 0) return TD_READ64;
    return 0;
}

/* 1 if the card driver says unit 0 is write-protected (dtoverlay=emmc,unit0=ro) */
static int probe_ro(struct card *c)
{
    struct IOStdReq *io = c->io;
    io->req.io_Command = TD_PROTSTATUS;
    io->io_Data = 0; io->io_Length = 0; io->io_Actual = 0; io->io_Offset = 0;
    return DoIO(io) == 0 && io->io_Actual != 0;
}

/* ---- mounting ---------------------------------------------------------------- */
static int bstr_eq(ULONG bptr, const char *s)
{
    const UBYTE *b = (const UBYTE *)(bptr << 2);
    unsigned n = 0;
    while (s[n]) n++;
    if (!b || b[0] != n) return 0;
    for (unsigned i = 0; i < n; i++) {
        UBYTE a = b[1 + i], c = (UBYTE)s[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (a != c) return 0;
    }
    return 1;
}

static struct List *mountlist(APTR eb)
{
    struct List *ml = (struct List *)((UBYTE *)eb + EB_MOUNTLIST);
    if (ml->lh_Tail || !ml->lh_Head || ((ULONG)ml->lh_Head & 1)) return 0;   /* not a list: do not walk it */
    return ml;
}

/* the boot node of a drive with this name, if one is mounted already */
static struct BootNode *find_drive(APTR eb, const char *name)
{
    struct List *ml = mountlist(eb);
    if (!ml) return 0;
    int guard = 256;
    for (struct Node *n = ml->lh_Head; n && !((ULONG)n & 1) && n->ln_Succ && guard--; n = n->ln_Succ) {
        struct DeviceNode *dn = ((struct BootNode *)n)->bn_DeviceNode;
        if (dn && !((ULONG)dn & 1) && dn->dn_Name && bstr_eq(dn->dn_Name, name)) return (struct BootNode *)n;
    }
    return 0;
}

static LONG highest_boot_pri(APTR eb)
{
    struct List *ml = mountlist(eb);
    LONG best = -128;
    if (!ml) return best;
    int guard = 256;
    for (struct Node *n = ml->lh_Head; n && !((ULONG)n & 1) && n->ln_Succ && guard--; n = n->ln_Succ)
        if (n->ln_Pri > best) best = n->ln_Pri;
    return best;
}

static void patch_fs(struct DeviceNode *dn, ULONG dostype)
{
    struct FileSysResource *fsr = OpenResource("FileSystem.resource");
    if (!fsr) return;
    struct FileSysEntry *best = 0;
    for (struct Node *n = fsr->fsr_FileSysEntries.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct FileSysEntry *e = (struct FileSysEntry *)n;
        if (e->fse_DosType == dostype && (!best || e->fse_Version > best->fse_Version)) best = e;
    }
    if (!best) return;
    ULONG *dst = &dn->dn_Type, *src = &best->fse_Type;
    for (int i = 0; i < 9; i++)                        /* dn_Type .. dn_GlobalVec */
        if (best->fse_PatchFlags & (1u << i)) dst[i] = src[i];
}

static int fs_known(ULONG dostype)
{
    if ((dostype & 0xffffff00u) == 0x444f5300u && (dostype & 0xff) <= 7) return 1;   /* DOS\0..7: the ROM's FFS */
    struct FileSysResource *fsr = OpenResource("FileSystem.resource");
    if (!fsr) return 0;
    for (struct Node *n = fsr->fsr_FileSysEntries.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (((struct FileSysEntry *)n)->fse_DosType == dostype) return 1;
    return 0;
}

static void set_name(char *d, const char *s)
{
    unsigned k = 0;
    for (; s[k] && k < 31; k++) d[k] = s[k];
    d[k] = 0;
}

static void mount_unit(struct HdfBase *db, struct card *c, APTR eb, APTR cd, struct HUnit *hu, int any_chosen)
{
    struct hdf_io io = HDF_IO(c);
    for (int i = 0; i < hu->rdb.nparts; i++) {
        struct hdf_part *p = &hu->rdb.part[i];
        ULONG *ev = p->envec;
        if (p->flags & 2) { hdf_log(&io, "  %s: marked no-mount", p->name); continue; }
        if (ev[0] < 11) { hdf_log(&io, "  %s: environment too short", p->name); continue; }
        ULONG dostype = ev[0] >= 16 ? ev[16] : 0x444f5300u;
        ULONG bpc = ev[3] * ev[5];                      /* heads * blocks per track */
        ULONG blocks = bpc * (ev[10] - ev[9] + 1);
        if (ev[1] != 128 || ev[4] != 1) { hdf_log(&io, "  %s: only 512-byte blocks are supported", p->name); continue; }
        if (ev[9] > ev[10] || bpc * (ev[10] + 1) > hu->f.blocks) { hdf_log(&io, "  %s: extends past the end of the file", p->name); continue; }
        char fsname[16];
        fsname[0] = (char)(dostype >> 24); fsname[1] = (char)(dostype >> 16); fsname[2] = (char)(dostype >> 8);
        fsname[3] = '\\'; fsname[4] = (char)('0' + (dostype & 0xff) % 10); fsname[5] = 0;
        if (!fs_known(dostype)) {
            hdf_log(&io, "  %s: filesystem %s not loaded - left out (filesystems from the RDB: phase 3)", p->name[0] ? p->name : "partition", fsname);
            continue;
        }

        /* the name: the RDB's, or for a single-partition hardfile DH0, DH1, ... */
        char name[32];
        struct BootNode *other = 0;
        if (hu->single) {
            set_name(name, "DH0");
            for (int k = 0; k < 10; k++) {
                name[2] = (char)('0' + k);
                other = find_drive(eb, name);
                if (!other || hu->chosen) break;        /* the chosen one takes DH0 */
            }
            if (!hu->chosen) other = find_drive(eb, name);
        } else {
            set_name(name, p->name[0] ? p->name : "DH0");
            other = find_drive(eb, name);
        }
        if (other) {
            if (!hu->chosen) {
                char hint[HDF_NAME];                    /* cmdline.txt splits at spaces: '_' stands for ' ' */
                unsigned k = 0;
                for (; hu->f.name[k] && k < HDF_NAME - 1; k++) hint[k] = hu->f.name[k] == ' ' ? '_' : hu->f.name[k];
                hint[k] = 0;
                hdf_log(&io, "  %s: a drive of that name is mounted already - not mounted (hdf.boot=%s in cmdline.txt mounts this one instead)",
                        name, hint);
                continue;
            }
            Forbid();
            Remove(&other->bn_Node);                    /* the other drive is not mounted this time */
            Permit();
            hdf_log(&io, "  %s: replaces the drive mounted as %s (hdf.boot)", name, name);
        }

        ULONG pkt[4 + 20];
        pkt[0] = (ULONG)name; pkt[1] = (ULONG)hdf_devname; pkt[2] = hu->index; pkt[3] = 0;
        for (ULONG j = 0; j <= ev[0] && j < 20; j++) pkt[4 + j] = ev[j];
        struct DeviceNode *dn = MakeDosNode(eb, pkt);
        if (!dn) { hdf_log(&io, "  %s: MakeDosNode failed", name); continue; }
        patch_fs(dn, dostype);

        int boot;
        LONG pri;
        if (hu->chosen) {                               /* boots, ahead of every other drive */
            boot = (p->flags & 1) || hu->single;
            pri = (p->flags & 1) ? (LONG)ev[15] : 0;
            if (boot) {
                LONG top = highest_boot_pri(eb);
                if (pri <= top) pri = top + 1;
                if (pri > 127) pri = 127;
            }
        } else {                                        /* its RDB decides, unless another one was chosen */
            boot = (p->flags & 1) && !hu->single && !any_chosen;
            pri = (LONG)ev[15];
        }
        if (!boot) pri = -128;
        else if (pri < -127) pri = -127;
        if (!AddBootNode(eb, pri, 1 /* ADNF_STARTPROC */, dn, cd)) { hdf_log(&io, "  %s: AddBootNode failed", name); continue; }
        if (boot) hdf_log(&io, "  %s: %lu MB, %s, boot priority %ld%s", name, blocks >> 11, fsname, pri, db->readonly ? ", read-only" : "");
        else      hdf_log(&io, "  %s: %lu MB, %s, not bootable%s", name, blocks >> 11, fsname, db->readonly ? ", read-only" : "");
    }
}

static int matches_boot(const struct hdf_opts *o, const char *file)
{
    if (!o->boot[0]) return 0;
    if (hdf_name_match(file, o->boot)) return 1;
    char base[HDF_NAME];                                /* hdf.boot=Workbench for Workbench.hdf */
    unsigned k = 0;
    for (; file[k] && k < HDF_NAME - 1; k++) base[k] = file[k];
    base[k] = 0;
    if (k > 4 && base[k - 4] == '.') base[k - 4] = 0;
    return hdf_name_match(base, o->boot);
}

/* ---- device ------------------------------------------------------------------- */
APTR dev_init(struct HdfBase *db, APTR seglist)
{
    (void)seglist;
    db->lib.lib_Node.ln_Type = NT_DEVICE;
    db->lib.lib_Node.ln_Name = (char *)hdf_devname;
    db->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    db->lib.lib_Version = VERSION;
    db->lib.lib_Revision = REVISION;
    db->lib.lib_IdString = (APTR)hdf_idstring;
    db->magic = HDF_MAGIC;

    struct card c;
    struct MsgPort *rp;
    c.db = db;
    struct hdf_io io = HDF_IO(&c);
    hdf_log(&io, "%s", hdf_idstring);
    if (card_open(db, &c, &rp)) {
        hdf_log(&io, "the whole SD card (brcm-emmc.device unit 0) is not available: add dtoverlay=emmc,unit0=rw (or ro) to config.txt");
        return db;
    }
    APTR buf = AllocMem(HDF_BLOCK, MEMF_PUBLIC | MEMF_CLEAR);
    db->cmd64 = buf ? probe64(&c, buf) : 0;
    if (buf) FreeMem(buf, HDF_BLOCK);
    int card_ro = probe_ro(&c);
    hdf_log(&io, "card: %s unit 0, %s%s", db->card_dev,
            db->cmd64 == NSCMD_TD_READ64 ? "NSD 64-bit" : db->cmd64 == TD_READ64 ? "TD64" : "32-bit only (first 4 GB)",
            card_ro ? ", read-only (unit0=ro)" : "");

    struct fat f;
    struct hdf_file *files = AllocMem(sizeof(struct hdf_file) * HDF_MAX_FILES, MEMF_PUBLIC | MEMF_CLEAR);
    char *cmdline = AllocMem(1024, MEMF_PUBLIC | MEMF_CLEAR);
    int n = 0;
    if (files && cmdline && fat_open(&f, &io) == 0) {
        n = fat_find_hdfs(&f, files, HDF_MAX_FILES);
        if (fat_read_cmdline(&f, cmdline, 1024) < 0) cmdline[0] = 0;
        fat_close(&f);
    }
    hdf_parse_opts(cmdline ? cmdline : "", &db->opts);
    if (cmdline) FreeMem(cmdline, 1024);
    db->readonly = db->opts.readonly || card_ro;
    if (db->opts.off || db->opts.readonly || db->opts.boot[0])
        hdf_log(&io, "cmdline.txt:%s%s%s%s", db->opts.off ? " hdf=off" : "", db->opts.readonly ? " hdf.readonly" : "",
                db->opts.boot[0] ? " hdf.boot=" : "", db->opts.boot);
    if (db->opts.off) {
        hdf_log(&io, "hardfiles turned off (hdf=off)");
        for (int i = 0; i < n; i++) hdf_file_free(&io, &files[i]);
        n = 0;
    } else if (!n) {
        hdf_log(&io, "no hardfiles (*.hdf) in the FAT partition's root or HDF folder");
    }

    int any_chosen = 0;
    for (int i = 0; i < n; i++) any_chosen |= matches_boot(&db->opts, files[i].name);
    if (db->opts.boot[0] && !any_chosen && !db->opts.off) hdf_log(&io, "hdf.boot=%s: no hardfile of that name", db->opts.boot);

    APTR eb = OpenLibrary("expansion.library", 36);
    struct ConfigDev *cd = eb ? FindConfigDev(eb, 0, 0x6d73, 2) : 0;       /* Emu68's ROM board */
    /* the chosen hardfile first, so that it gets its names before the others ask */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            struct hdf_file *h = &files[i];
            if (!h->ext || (matches_boot(&db->opts, h->name) != (pass == 0))) continue;
            uint32_t top = 0;
            for (uint32_t k = 0; k < h->next; k++)
                if (h->ext[k].card_sec + h->ext[k].count > top) top = h->ext[k].card_sec + h->ext[k].count;
            if (!db->cmd64 && top > 0x800000u) {
                hdf_log(&io, "%s: lies beyond the card's first 4 GB, which %s cannot reach", h->name, db->card_dev);
                hdf_file_free(&io, h);
                continue;
            }
            struct HUnit *hu = AllocMem(sizeof *hu, MEMF_PUBLIC | MEMF_CLEAR);
            if (!hu || db->nunits >= HDF_MAX_FILES) { hdf_file_free(&io, h); continue; }
            hu->f = *h;
            h->ext = 0;                                     /* the unit owns the extents now */
            hu->index = db->nunits;
            hu->chosen = pass == 0;
            int r = hdf_rdb(&io, &hu->f, &hu->rdb);
            if (r == 0 && hdf_single(&io, &hu->f, &hu->rdb) == 1) { hu->single = 1; r = 1; }
            if (r == 1) {
                hdf_log(&io, "unit %lu: %s, %s%s", hu->index, hu->f.name,
                        hu->single ? "single partition" : (hu->rdb.nparts == 1 ? "1 partition" : "RDB"), hu->chosen ? ", hdf.boot" : "");
                db->unit[db->nunits++] = hu;
                if (eb) mount_unit(db, &c, eb, cd, hu, any_chosen);
            } else {
                hdf_log(&io, "%s: %s", hu->f.name, r == 0 ? "neither a Rigid Disk Block nor a filesystem at block 0 - ignored" : "unreadable RDB");
                hdf_file_free(&io, &hu->f);
                FreeMem(hu, sizeof *hu);
            }
        }
    }
    if (eb) CloseLibrary(eb);
    if (files) FreeMem(files, sizeof(struct hdf_file) * HDF_MAX_FILES);
    card_close(&c, rp);

    if (db->nunits) {                                   /* the I/O task */
        db->stack = AllocMem(TASK_STACK, MEMF_PUBLIC | MEMF_CLEAR);
        if (!db->stack) { hdf_log(&io, "no memory for the task"); db->nunits = 0; return db; }
        struct Task *t = &db->task;
        t->tc_Node.ln_Type = NT_TASK;
        t->tc_Node.ln_Pri = 5;
        t->tc_Node.ln_Name = (char *)hdf_devname;
        t->tc_SPLower = db->stack;
        t->tc_SPUpper = (UBYTE *)db->stack + TASK_STACK;
        t->tc_SPReg = t->tc_SPUpper;
        t->tc_SigAlloc = 0xffffu | (1u << 16);            /* bit 16: the request port */
        t->tc_UserData = db;
        NewList(&t->tc_MemEntry);
        db->port.mp_Node.ln_Type = NT_MSGPORT;
        db->port.mp_Flags = PA_SIGNAL;
        db->port.mp_SigBit = 16;
        db->port.mp_SigTask = t;
        NewList(&db->port.mp_MsgList);
        add_task(t, (APTR)task_entry);
    }
    hdf_log(&io, "%lu unit%s ready%s", db->nunits, db->nunits == 1 ? "" : "s", db->readonly ? ", read-only" : "");
    return db;
}

static void reply(struct IORequest *io)
{
    if (!(io->io_Flags & IOF_QUICK)) ReplyMsg(io);
}

LONG dev_open(struct HdfBase *db, struct IORequest *io, ULONG unit, ULONG flags)
{
    (void)flags;
    if (unit >= db->nunits || !db->unit[unit]) { io->io_Error = IOERR_OPENFAIL; io->io_Unit = 0; return IOERR_OPENFAIL; }
    io->io_Unit = &db->unit[unit]->u;
    io->io_Unit->unit_OpenCnt++;
    db->lib.lib_OpenCnt++;
    db->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    io->io_Error = 0;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    return 0;
}

APTR dev_close(struct HdfBase *db, struct IORequest *io)
{
    if (io->io_Unit && io->io_Unit != (struct Unit *)-1) io->io_Unit->unit_OpenCnt--;
    io->io_Unit = (struct Unit *)-1;
    io->io_Device = (APTR)-1;
    db->lib.lib_OpenCnt--;
    return 0;
}

static const UWORD supported[] = {
    CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, TD_MOTOR, TD_FORMAT, TD_REMOVE, TD_CHANGENUM, TD_CHANGESTATE,
    TD_PROTSTATUS, TD_GETDRIVETYPE, TD_GETNUMTRACKS, TD_ADDCHANGEINT, TD_REMCHANGEINT, TD_GETGEOMETRY, TD_EJECT,
    TD_READ64, TD_WRITE64, TD_FORMAT64, NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_FORMAT64, 0
};

void dev_beginio(struct HdfBase *db, struct IORequest *ior)
{
    struct IOStdReq *io = (struct IOStdReq *)ior;
    struct HUnit *hu = (struct HUnit *)ior->io_Unit;
    ior->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ior->io_Error = 0;
    switch (ior->io_Command) {
    case CMD_WRITE: case TD_FORMAT: case TD_WRITE64: case TD_FORMAT64: case NSCMD_TD_WRITE64: case NSCMD_TD_FORMAT64:
        if (db->readonly) { io->io_Actual = 0; ior->io_Error = TDERR_WriteProt; break; }
        __attribute__((fallthrough));                  /* the task does it */
    case CMD_READ: case TD_READ64: case NSCMD_TD_READ64:
        ior->io_Flags &= (UBYTE)~IOF_QUICK;
        PutMsg(&db->port, ior);
        return;
    case TD_PROTSTATUS:  io->io_Actual = db->readonly ? 1 : 0; break;
    case TD_CHANGESTATE: io->io_Actual = 0; break;     /* a disk is in */
    case TD_CHANGENUM:   io->io_Actual = 1; break;
    case TD_GETDRIVETYPE: io->io_Actual = DRIVE3_5; break;
    case TD_GETNUMTRACKS: io->io_Actual = hu->rdb.cylinders * hu->rdb.heads; break;
    case CMD_UPDATE: case CMD_CLEAR: case TD_MOTOR: case TD_REMOVE: case TD_EJECT: case TD_REMCHANGEINT:
        io->io_Actual = 0; break;                      /* no cache here: writes are on the card when replied */
    case TD_ADDCHANGEINT:                              /* kept, never fires: the disk cannot change */
        ior->io_Flags &= (UBYTE)~IOF_QUICK;
        return;
    case TD_GETGEOMETRY: {
        struct DriveGeometry *g = io->io_Data;
        if (!g || io->io_Length < sizeof *g) { ior->io_Error = IOERR_BADLENGTH; break; }
        memset(g, 0, sizeof *g);
        g->dg_SectorSize = HDF_BLOCK;
        g->dg_TotalSectors = hu->f.blocks;
        g->dg_Heads = hu->rdb.heads;
        g->dg_TrackSectors = hu->rdb.sectors;
        g->dg_CylSectors = hu->rdb.heads * hu->rdb.sectors;
        g->dg_Cylinders = g->dg_CylSectors ? hu->f.blocks / g->dg_CylSectors : 0;
        g->dg_BufMemType = MEMF_PUBLIC;
        io->io_Actual = sizeof *g;
        break;
    }
    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *q = io->io_Data;
        if (!q || io->io_Length < 16) { ior->io_Error = IOERR_BADLENGTH; break; }
        q->DevQueryFormat = 0;
        q->SizeAvailable = 16;
        q->DeviceType = 5;                             /* NSDEVTYPE_TRACKDISK */
        q->DeviceSubType = 0;
        q->SupportedCommands = supported;
        io->io_Actual = 16;
        break;
    }
    default:
        ior->io_Error = IOERR_NOCMD;
        break;
    }
    reply(ior);
}

LONG dev_abortio(struct HdfBase *db, struct IORequest *ior)
{
    (void)db; (void)ior;
    return 0;                                          /* transfers finish quickly; change ints just stay */
}

/* ---- the I/O task --------------------------------------------------------------- */
static void do_io(struct card *c, struct IOStdReq *io)
{
    struct HUnit *hu = (struct HUnit *)io->req.io_Unit;
    UWORD cmd = io->req.io_Command;
    int write = !(cmd == CMD_READ || cmd == TD_READ64 || cmd == NSCMD_TD_READ64);
    uint32_t hi = (cmd == CMD_READ || cmd == CMD_WRITE || cmd == TD_FORMAT) ? 0 : io->io_Actual;
    uint32_t lo = io->io_Offset, len = io->io_Length;
    io->io_Actual = 0;
    if ((lo | len) & (HDF_BLOCK - 1)) { io->req.io_Error = IOERR_BADLENGTH; return; }
    if (hi >> 9) { io->req.io_Error = IOERR_BADADDRESS; return; }
    uint32_t blk = (hi << 23) | (lo >> 9), cnt = len >> 9;
    if (blk > hu->f.blocks || cnt > hu->f.blocks - blk) { io->req.io_Error = IOERR_BADADDRESS; return; }
    struct hdf_io hio = HDF_IO(c);
    if (cnt) {
        int r = write ? hdf_write(&hio, &hu->f, blk, cnt, io->io_Data) : hdf_read(&hio, &hu->f, blk, cnt, io->io_Data);
        if (r == -2) {                                 /* the card turned out to be read-only */
            c->db->readonly = 1;
            hdf_log(&hio, "the card refused a write: %s unit 0 is read-only (dtoverlay=emmc,unit0=rw)", c->db->card_dev);
            io->req.io_Error = TDERR_WriteProt;
            return;
        }
        if (r) { io->req.io_Error = TDERR_NotSpecified; return; }
    }
    io->io_Actual = len;
}

/* one pass over the queue; split out so the 68k test harness can drive it */
void hdf_service(struct HdfBase *db, struct card *c)
{
    struct IOStdReq *io;
    while ((io = GetMsg(&db->port))) {
        do_io(c, io);
        ReplyMsg(io);
    }
}

void hdf_task(struct HdfBase *db)
{
    struct card c;
    struct MsgPort *rp;
    struct hdf_io io = HDF_IO(&c);
    c.db = db;
    (void)AllocSignal(16);           /* -1 if AddTask kept tc_SigAlloc: fine. Requests that came
                                        before are still queued: the first pass below takes them */
    if (card_open(db, &c, &rp)) {                          /* refuse everything rather than hang the filesystems */
        hdf_log(&io, "task: cannot open %s", db->card_dev);
        for (;;) {
            struct IOStdReq *r;
            while ((r = GetMsg(&db->port))) { r->req.io_Error = IOERR_OPENFAIL; ReplyMsg(r); }
            Wait(1u << 16);
        }
    }
    for (;;) {
        hdf_service(db, &c);
        Wait(1u << 16);
    }
}

/* ---- the auto-init table -------------------------------------------------------- */
extern void stub_init(void), stub_open(void), stub_close(void), stub_expunge(void), stub_null(void), stub_beginio(void), stub_abortio(void);
const APTR dev_functab[] = {
    (APTR)stub_open, (APTR)stub_close, (APTR)stub_expunge, (APTR)stub_null,
    (APTR)stub_beginio, (APTR)stub_abortio, (APTR)-1
};
const ULONG dev_inittab[4] = { sizeof(struct HdfBase), (ULONG)dev_functab, 0, (ULONG)stub_init };
