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
 * Every hardfile is mounted; none boots unless cmdline.txt (read from the
 * FAT partition) says hdf.boot=NAME. That one boots, ahead of the other
 * drives, and where one of its partitions has the name of a drive already
 * mounted (an SD card partition, say DH0) it takes the name over and that
 * drive is not mounted. Other hardfile partitions whose names are taken get
 * HF<unit><part>. Also: hdf.readonly, hdf=off.
 *
 * Hardfile writes go to the file's own clusters on the card, nowhere else; the FAT
 * itself is never changed. Partitions whose filesystem is not FFS and not
 * already in FileSystem.resource are left out.
 * One task does the card I/O, in arrival order; the filesystems' buffers are
 * handed straight to brcm-emmc.device (no copy).
 *
 * Folders of the FAT partition (config.txt alice68.folder=..., and ALICE68)
 * are mounted as read-write volumes: folders.c, with the FAT engine fatfs.c.
 * Through them the FAT is written; hardfiles that are mounted stay read-only.
 *
 * CD images (*.iso in the root and in a root folder ISO) are mounted as
 * read-only volumes CD0:, CD1:, ...: isos.c, with iso9660.c. cmdline.txt
 * iso=off turns them off.
 *
 * Everything writable lives in the device base: the code runs from the ROM
 * board, which is mapped read-only.
 */

#define VERSION   0
#define REVISION  7
const char hdf_devname[] = "alice68hdf.device";
const char hdf_idstring[] = "alice68hdf.device 0.7 (09.11.2026) Alice68 hardfiles, folders and CD images";

#include "hdf_dev.h"

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
static void RawPutChar(char c)                        { lvo(SYSBASE, -516, (ULONG)(UBYTE)c, 0, 0, 0); }
static void CacheClearU(void)                         { lvo(SYSBASE, -636, 0, 0, 0, 0); }
extern void add_task(struct Task *t, APTR initpc);
extern void task_entry(void);

static void NewList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = 0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

/* ---- log ------------------------------------------------------------------ */
void log_line(void *ctx, const char *line)
{
    struct HdfBase *db = ((struct card *)ctx)->db;
    const char *p = "[alice68hdf] ";
    Forbid();
    for (int pass = 0; pass < 2; pass++, p = line)
        for (; *p; p++) { db->log[db->log_wr++ % LOG_SIZE] = *p; RawPutChar(*p); }
    db->log[db->log_wr++ % LOG_SIZE] = '\n';
    RawPutChar('\n');                                   /* the Alice68 kernel puts [alice68...] lines on the splash and serial log */
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

int card_read(void *ctx, uint32_t sector, uint32_t count, void *buf)
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

int card_write(void *ctx, uint32_t sector, uint32_t count, const void *buf)
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
void *core_alloc(void *ctx, uint32_t n) { (void)ctx; return AllocMem(n, MEMF_PUBLIC | MEMF_CLEAR); }
void core_free(void *ctx, void *p, uint32_t n) { (void)ctx; FreeMem(p, n); }

int card_open(struct HdfBase *db, struct card *c, struct MsgPort **port)
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

void card_close(struct card *c, struct MsgPort *port)
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
struct BootNode *find_drive(APTR eb, const char *name)
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

static ULONG fs_version(struct HdfBase *db, ULONG dostype);

static void patch_fs(struct HdfBase *db, struct DeviceNode *dn, ULONG dostype)
{
    ULONG ver = 0, flags = 0, *src = 0;
    struct FileSysResource *fsr = OpenResource("FileSystem.resource");
    if (fsr)
        for (struct Node *n = fsr->fsr_FileSysEntries.lh_Head; n->ln_Succ; n = n->ln_Succ) {
            struct FileSysEntry *e = (struct FileSysEntry *)n;
            if (e->fse_DosType == dostype && (!src || e->fse_Version > ver)) { ver = e->fse_Version; flags = e->fse_PatchFlags; src = &e->fse_Type; }
        }
    for (ULONG i = 0; i < db->nlfs; i++)
        if (db->lfs[i].dostype == dostype && (!src || db->lfs[i].version > ver)) { ver = db->lfs[i].version; flags = db->lfs[i].patchflags; src = db->lfs[i].dn; }
    if (!src) return;
    ULONG *dst = &dn->dn_Type;
    for (int i = 0; i < 9; i++)                        /* dn_Type .. dn_GlobalVec */
        if (flags & (1u << i)) dst[i] = src[i];
}

static int fs_known(struct HdfBase *db, ULONG dostype)
{
    if ((dostype & 0xffffff00u) == 0x444f5300u && (dostype & 0xff) <= 7) return 1;   /* DOS\0..7: the ROM's FFS */
    return fs_version(db, dostype) != 0;
}

int str_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

int find_vol(struct HdfBase *db, const char *vol)
{
    for (ULONG i = 0; i < db->nvols; i++) if (str_ieq(db->vols[i].vol, vol)) return (int)i;
    return -1;
}
void add_vol(struct HdfBase *db, const char *vol, const char *drive)
{
    if (db->nvols >= sizeof db->vols / sizeof db->vols[0]) return;
    unsigned k;
    for (k = 0; vol[k] && k < 31; k++) db->vols[db->nvols].vol[k] = vol[k];
    db->vols[db->nvols].vol[k] = 0;
    for (k = 0; drive[k] && k < 31; k++) db->vols[db->nvols].drive[k] = drive[k];
    db->vols[db->nvols].drive[k] = 0;
    db->nvols++;
}
static void drop_vols_of(struct HdfBase *db, const char *drive)
{
    for (ULONG i = 0; i < db->nvols; )
        if (str_ieq(db->vols[i].drive, drive)) db->vols[i] = db->vols[--db->nvols];
        else i++;
}

/* the SD card's own Amiga partitions (0x76): their volumes, as mounted by brcm-emmc.device */
static void sd_volumes(struct HdfBase *db, struct hdf_io *io)
{
    struct hdf_file parts[4];
    int n = card_amiga_parts(io, parts, 4);
    struct hdf_rdb *r = io->alloc(io->ctx, sizeof *r);
    for (int i = 0; i < n && r; i++) {
        if (hdf_rdb(io, &parts[i], r) == 1) {
            for (int k = 0; k < r->nparts; k++) {
                char vol[32];
                if ((r->part[k].flags & 2) || !hdf_volume(io, &parts[i], &r->part[k], vol)) continue;
                add_vol(db, vol, r->part[k].name);
                hdf_log(io, "%s: %s is volume %s", parts[i].name, r->part[k].name, vol);
            }
        }
        hdf_file_free(io, &parts[i]);
    }
    if (r) io->free(io->ctx, r, sizeof *r);
}

/* ---- loading a filesystem from an RDB: our own LoadSeg, dos.library is not up yet ---- */
#define HUNK_HEADER 0x3f3
#define HUNK_CODE   0x3e9
#define HUNK_DATA   0x3ea
#define HUNK_BSS    0x3eb
#define HUNK_RELOC32 0x3ec
#define HUNK_SYMBOL 0x3f0
#define HUNK_DEBUG  0x3f1
#define HUNK_END    0x3f2
#define HUNK_DREL32 0x3f7
#define HUNK_RELOC32SHORT 0x3fc
#define MAXHUNKS    32

static void free_seglist(ULONG seg)
{
    while (seg) {
        ULONG *m = (ULONG *)(seg << 2) - 1;             /* the size long before the link */
        ULONG next = m[1];
        FreeMem(m, m[0]);
        seg = next;
    }
}

/* a load file in memory -> a seglist (BPTR), 0 if it is not one we can load */
static ULONG load_seg(const UBYTE *f, ULONG size)
{
    const ULONG *w = (const ULONG *)f;
    ULONG nw = size / 4, i = 0;
    ULONG *hunk[MAXHUNKS];
    ULONG hsize[MAXHUNKS];
    if (nw < 6 || (ULONG)f & 1 || w[i++] != HUNK_HEADER || w[i++] != 0) return 0;      /* no resident libraries */
    ULONG tab = w[i++], first = w[i++], last = w[i++];
    (void)tab;
    if (first != 0 || last < first || last - first + 1 > MAXHUNKS) return 0;
    ULONG n = last - first + 1, seg = 0;
    for (ULONG h = 0; h < n; h++) hunk[h] = 0;
    for (ULONG h = 0; h < n; h++) {
        if (i >= nw) goto bad;
        ULONG sz = w[i++], req = MEMF_PUBLIC | MEMF_CLEAR;
        if ((sz & 0xc0000000u) == 0xc0000000u) { if (i >= nw) goto bad; req = (w[i++] & ~0x10000u) | MEMF_CLEAR; }
        else if (sz & 0x40000000u) req |= 2;            /* MEMF_CHIP */
        sz &= 0x3fffffffu;
        hsize[h] = sz;
        ULONG bytes = sz * 4 + 8;
        ULONG *m = AllocMem(bytes, req);
        if (!m) goto bad;
        m[0] = bytes;
        m[1] = 0;
        hunk[h] = m + 2;
        if (h) hunk[h - 1][-1] = (ULONG)(m + 1) >> 2;   /* link the previous one to this */
        else seg = (ULONG)(m + 1) >> 2;
    }
    ULONG cur = 0;
    while (i < nw && cur < n) {
        ULONG t = w[i++] & 0x3fffffffu;
        switch (t) {
        case HUNK_CODE: case HUNK_DATA: {
            if (i >= nw) goto bad;
            ULONG k = w[i++];
            if (k > hsize[cur] || i + k > nw) goto bad;
            memcpy(hunk[cur], &w[i], k * 4);
            i += k;
            break;
        }
        case HUNK_BSS:
            if (i >= nw) goto bad;
            i++;                                        /* already zeroed */
            break;
        case HUNK_RELOC32:
            for (;;) {
                if (i >= nw) goto bad;
                ULONG k = w[i++];
                if (!k) break;
                if (i >= nw || w[i] >= n || i + 1 + k > nw) goto bad;
                ULONG base = (ULONG)hunk[w[i++]];
                for (ULONG r = 0; r < k; r++) {
                    ULONG off = w[i++];
                    if (off + 4 > hsize[cur] * 4) goto bad;
                    UBYTE *p = (UBYTE *)hunk[cur] + off;
                    ULONG v = ((ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]) + base;
                    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16); p[2] = (UBYTE)(v >> 8); p[3] = (UBYTE)v;
                }
            }
            break;
        case HUNK_RELOC32SHORT: case HUNK_DREL32: {
            const UWORD *h16 = (const UWORD *)&w[i];
            ULONG j = 0, max16 = (nw - i) * 2;
            for (;;) {
                if (j >= max16) goto bad;
                ULONG k = h16[j++];
                if (!k) break;
                if (j + 1 + k > max16 || h16[j] >= n) goto bad;
                ULONG base = (ULONG)hunk[h16[j++]];
                for (ULONG r = 0; r < k; r++) {
                    ULONG off = h16[j++];
                    if (off + 4 > hsize[cur] * 4) goto bad;
                    UBYTE *p = (UBYTE *)hunk[cur] + off;
                    ULONG v = ((ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]) + base;
                    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16); p[2] = (UBYTE)(v >> 8); p[3] = (UBYTE)v;
                }
            }
            i += (j + 1) / 2;                           /* padded to a long */
            break;
        }
        case HUNK_SYMBOL:
            for (;;) {
                if (i >= nw) goto bad;
                ULONG k = w[i++];
                if (!k) break;
                i += k + 1;
            }
            break;
        case HUNK_DEBUG:
            if (i >= nw) goto bad;
            i += w[i] + 1;
            break;
        case HUNK_END:
            cur++;
            break;
        default:
            goto bad;                                   /* overlays, HUNK_EXT, ...: not in a filesystem */
        }
    }
    if (cur != n) goto bad;
    CacheClearU();                                      /* new code: Emu68 must translate it afresh */
    return seg;
bad:
    if (seg) free_seglist(seg);
    else if (hunk[0]) FreeMem(hunk[0] - 2, hunk[0][-2]);
    return 0;
}

/* the newest version of a filesystem: in FileSystem.resource or loaded by us; 0 none */
static ULONG fs_version(struct HdfBase *db, ULONG dostype)
{
    ULONG best = 0;
    struct FileSysResource *fsr = OpenResource("FileSystem.resource");
    if (fsr)
        for (struct Node *n = fsr->fsr_FileSysEntries.lh_Head; n->ln_Succ; n = n->ln_Succ) {
            struct FileSysEntry *e = (struct FileSysEntry *)n;
            if (e->fse_DosType == dostype && e->fse_Version > best) best = e->fse_Version;
        }
    for (ULONG i = 0; i < db->nlfs; i++)
        if (db->lfs[i].dostype == dostype && db->lfs[i].version > best) best = db->lfs[i].version;
    return best;
}

static void load_rdb_filesystems(struct HdfBase *db, struct hdf_io *io, struct HUnit *hu)
{
    struct hdf_fs fs[8];
    int n = hdf_rdb_fslist(io, &hu->f, &hu->rdb, fs, 8);
    for (int i = 0; i < n; i++) {
        struct hdf_fs *f = &fs[i];
        char t[8];
        t[0] = (char)(f->dostype >> 24); t[1] = (char)(f->dostype >> 16); t[2] = (char)(f->dostype >> 8);
        t[3] = '\\'; t[4] = (char)('0' + (f->dostype & 0xff) % 10); t[5] = 0;
        ULONG have = fs_version(db, f->dostype);
        if (have >= f->version) continue;               /* a newer or the same one is there already */
        if (db->nlfs >= sizeof db->lfs / sizeof db->lfs[0]) break;
        UBYTE *data; uint32_t size, alloc;
        if (hdf_rdb_fsdata(io, &hu->f, f, &data, &size, &alloc)) { hdf_log(io, "  filesystem %s: unreadable load file", t); continue; }
        ULONG seg = load_seg(data, size);
        io->free(io->ctx, data, alloc);
        if (!seg) { hdf_log(io, "  filesystem %s %lu.%lu: not a load file this can load", t, f->version >> 16, f->version & 0xffff); continue; }
        ULONG k = db->nlfs++;
        db->lfs[k].dostype = f->dostype; db->lfs[k].version = f->version; db->lfs[k].patchflags = f->patchflags;
        for (int j = 0; j < 9; j++) db->lfs[k].dn[j] = f->dn[j];
        db->lfs[k].dn[7] = seg;                         /* dn_SegList: the loaded code, not the LSEG block */
        /* for other drivers too, if FileSystem.resource is up */
        struct FileSysResource *fsr = OpenResource("FileSystem.resource");
        if (fsr) {
            struct FileSysEntry *e = AllocMem(62, MEMF_PUBLIC | MEMF_CLEAR);
            if (e) {
                e->fse_Node.ln_Name = (char *)hdf_devname;
                e->fse_DosType = f->dostype; e->fse_Version = f->version; e->fse_PatchFlags = f->patchflags;
                ULONG *dst = &e->fse_Type;
                for (int j = 0; j < 9; j++) dst[j] = db->lfs[k].dn[j];
                Forbid();
                struct List *l = &fsr->fsr_FileSysEntries;      /* AddHead */
                e->fse_Node.ln_Succ = l->lh_Head; e->fse_Node.ln_Pred = (struct Node *)&l->lh_Head;
                l->lh_Head->ln_Pred = &e->fse_Node; l->lh_Head = &e->fse_Node;
                Permit();
            }
        }
        hdf_log(io, "  filesystem %s %lu.%lu loaded from the RDB%s", t, f->version >> 16, f->version & 0xffff,
                have ? " (newer than the one already there)" : "");
    }
}

static void set_name(char *d, const char *s)
{
    unsigned k = 0;
    for (; s[k] && k < 31; k++) d[k] = s[k];
    d[k] = 0;
}

/* returns: bit 0 a partition was mounted, bit 1 one of them boots */
static int mount_unit(struct HdfBase *db, struct card *c, APTR eb, APTR cd, struct HUnit *hu, int any_chosen)
{
    struct hdf_io io = HDF_IO(c);
    (void)any_chosen;                                   /* only the chosen one boots, chosen or not */
    int res = 0;
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
        if (!fs_known(db, dostype)) {
            hdf_log(&io, "  %s: filesystem %s is not loaded and not in the hardfile's RDB - left out", p->name[0] ? p->name : "partition", fsname);
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
        if (other && !hu->chosen) {                     /* it never boots: mount it under a name of its own */
            char old[32];
            set_name(old, name);
            name[0] = 'H'; name[1] = 'F'; name[2] = (char)('0' + hu->index % 10); name[3] = (char)('0' + i % 10); name[4] = 0;
            for (char x = 'A'; find_drive(eb, name) && x <= 'Z'; x++) { name[4] = x; name[5] = 0; }
            hdf_log(&io, "  %s: a drive of that name is mounted already - mounted as %s", old, name);
            other = 0;
        }
        if (other) {                                    /* the chosen hardfile takes the name over */
            Forbid();
            Remove(&other->bn_Node);                    /* the other drive is not mounted this time */
            Permit();
            drop_vols_of(db, name);
            hdf_log(&io, "  %s: replaces the drive mounted as %s (hdf.boot)", name, name);
        }

        /* the volume: never mounted twice (AmigaOS mixes up two volumes of one name) */
        char vol[32];
        if (hdf_volume(&io, &hu->f, p, vol)) {
            int v = find_vol(db, vol);
            if (v >= 0 && !hu->chosen) {
                char hint[HDF_NAME];                    /* cmdline.txt splits at spaces: '_' stands for ' ' */
                unsigned k = 0;
                for (; hu->f.name[k] && k < HDF_NAME - 1; k++) hint[k] = hu->f.name[k] == ' ' ? '_' : hu->f.name[k];
                hint[k] = 0;
                hdf_log(&io, "  %s: volume %s is mounted already (on %s) - not mounted. Relabel one of them, or boot this one with hdf.boot=%s",
                        name, vol, db->vols[v].drive, hint);
                continue;
            }
            if (v >= 0) {                               /* the chosen hardfile: the other drive goes */
                struct BootNode *o2 = find_drive(eb, db->vols[v].drive);
                if (o2) {
                    Forbid(); Remove(&o2->bn_Node); Permit();
                    hdf_log(&io, "  %s: volume %s replaces the drive %s that has it too (hdf.boot)", name, vol, db->vols[v].drive);
                }
                drop_vols_of(db, db->vols[v].drive);
            }
        }

        ULONG pkt[4 + 20];
        pkt[0] = (ULONG)name; pkt[1] = (ULONG)hdf_devname; pkt[2] = hu->index; pkt[3] = 0;
        for (ULONG j = 0; j <= ev[0] && j < 20; j++) pkt[4 + j] = ev[j];
        struct DeviceNode *dn = MakeDosNode(eb, pkt);
        if (!dn) { hdf_log(&io, "  %s: MakeDosNode failed", name); continue; }
        patch_fs(db, dn, dostype);

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
        } else {                                        /* only the hdf.boot one boots */
            boot = 0;
            pri = -128;
        }
        if (!boot) pri = -128;
        else if (pri < -127) pri = -127;
        if (!AddBootNode(eb, pri, 1 /* ADNF_STARTPROC */, dn, cd)) { hdf_log(&io, "  %s: AddBootNode failed", name); continue; }
        if (vol[0]) add_vol(db, vol, name);
        res |= 1 | (boot ? 2 : 0);
        if (boot) hdf_log(&io, "  %s: %lu MB, %s, boot priority %ld%s", name, blocks >> 11, fsname, pri, db->readonly ? ", read-only" : "");
        else      hdf_log(&io, "  %s: %lu MB, %s, not bootable%s", name, blocks >> 11, fsname, db->readonly ? ", read-only" : "");
    }
    return res;
}

/* The WiFi firmware for the kernel (src/alice/alice_net.c): the files found
   on the FAT partition are read and offered on the network board; the kernel
   copies them, and the buffers are freed again. */
#include "../../third_party/Emu68/src/alice/alice_net.h"
static ULONG pi_usecs(void)
{
    ULONG v = *(volatile ULONG *)0xF2003004u;
    return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24;
}
static void offer_wifi(struct fat *f, struct hdf_io *io)
{
    APTR eb = OpenLibrary("expansion.library", 36);
    struct ConfigDev *cd = eb ? FindConfigDev(eb, 0, AN_MANUFACTURER, AN_PRODUCT) : 0;
    if (eb) CloseLibrary(eb);
    if (!cd) return;
    volatile UBYTE *b = cd->cd_BoardAddr;
    volatile struct an_hdr *h = (volatile struct an_hdr *)b;
    if (h->magic != AN_MAGIC || h->fw_state == AN_FW_TAKEN) return;
    struct hdf_file *w = AllocMem(sizeof(struct hdf_file) * AN_FW_MAX, MEMF_PUBLIC | MEMF_CLEAR);
    if (!w) return;
    int n = fat_find_wifi(f, w, AN_FW_MAX);
    if (!n) { hdf_log(io, "WiFi: no firmware files (WIFI folder) on the FAT partition"); FreeMem(w, sizeof(struct hdf_file) * AN_FW_MAX); return; }
    UBYTE *buf[AN_FW_MAX];
    ULONG len[AN_FW_MAX];
    volatile struct an_fw *t = (volatile struct an_fw *)(b + AN_OFF_FW);
    int k = 0;
    for (int i = 0; i < n; i++) {
        len[k] = w[i].blocks * HDF_BLOCK;
        buf[k] = AllocMem(len[k], MEMF_PUBLIC);
        if (!buf[k] || hdf_read(io, &w[i], 0, w[i].blocks, buf[k])) {
            hdf_log(io, "WiFi: %s could not be read", w[i].name);
            if (buf[k]) FreeMem(buf[k], len[k]);
            hdf_file_free(io, &w[i]);
            continue;
        }
        for (int c = 0; c < 64; c++) t[k].name[c] = c < 63 ? w[i].name[c] : 0;
        t[k].addr = (ULONG)buf[k];
        t[k].len = w[i].size;
        hdf_log(io, "WiFi: %s, %lu bytes, for the kernel", w[i].name, w[i].size);
        hdf_file_free(io, &w[i]);
        k++;
    }
    FreeMem(w, sizeof(struct hdf_file) * AN_FW_MAX);
    if (!k) return;
    h->fw_count = (ULONG)k;
    __asm__ volatile("nop" ::: "memory");             /* Emu68: a full barrier - the table before the flag */
    h->fw_state = AN_FW_OFFERED;
    /* the kernel copies them on CPU1, as soon as its USB start-up lets it */
    ULONG t0 = pi_usecs();
    while (h->fw_state == AN_FW_OFFERED && pi_usecs() - t0 < 4000000u) ;
    if (h->fw_state == AN_FW_OFFERED) {
        hdf_log(io, "WiFi: the kernel has not taken the firmware yet - left in memory for it");
        return;                                         /* the buffers stay: the kernel takes them later */
    }
    for (int i = 0; i < k; i++) FreeMem(buf[i], len[i]);
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
    struct hdf_file *isos = AllocMem(sizeof(struct hdf_file) * ISO_MAX, MEMF_PUBLIC | MEMF_CLEAR);
    int ni = 0;
    char *cmdline = AllocMem(1024, MEMF_PUBLIC | MEMF_CLEAR);
    char *config = AllocMem(CONFIG_MAX, MEMF_PUBLIC | MEMF_CLEAR);
    int n = 0, have_fat = 0;
    if (files && cmdline && fat_open(&f, &io) == 0) {
        have_fat = 1;
        n = fat_find_hdfs(&f, files, HDF_MAX_FILES);
        if (isos) ni = fat_find_isos(&f, isos, ISO_MAX);
        if (fat_read_cmdline(&f, cmdline, 1024) < 0) cmdline[0] = 0;
        if (config && fat_read_config(&f, config, CONFIG_MAX) < 0) config[0] = 0;
        offer_wifi(&f, &io);
        fat_close(&f);
    }
    hdf_parse_opts(cmdline ? cmdline : "", &db->opts);
    if (cmdline) FreeMem(cmdline, 1024);
    db->readonly = db->opts.readonly || card_ro;
    if (db->opts.off || db->opts.readonly || db->opts.noboot || db->opts.boot[0] || db->opts.iso_off)
        hdf_log(&io, "cmdline.txt:%s%s%s%s%s%s", db->opts.off ? " hdf=off" : "", db->opts.readonly ? " hdf.readonly" : "",
                db->opts.noboot ? " hdf.noboot" : "", db->opts.iso_off ? " iso=off" : "", db->opts.boot[0] ? " hdf.boot=" : "", db->opts.boot);
    if (db->opts.off) {
        hdf_log(&io, "hardfiles turned off (hdf=off)");
        for (int i = 0; i < n; i++) hdf_file_free(&io, &files[i]);
        n = 0;
    } else if (!n) {
        hdf_log(&io, "no hardfiles (*.hdf) in the FAT partition's root or HDF folder");
    }

    int n_boot = 0, n_other = 0, n_left = 0;            /* hardfiles, for the summary line */
    int any_chosen = 0;
    for (int i = 0; i < n; i++) any_chosen |= matches_boot(&db->opts, files[i].name);
    if (db->opts.boot[0] && !any_chosen && !db->opts.off) hdf_log(&io, "hdf.boot=%s: no hardfile of that name", db->opts.boot);

    sd_volumes(db, &io);
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
                n_left++;
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
                if (!hu->single) load_rdb_filesystems(db, &io, hu);
                int m = eb ? mount_unit(db, &c, eb, cd, hu, any_chosen) : 0;
                if (m & 2) n_boot++; else if (m & 1) n_other++; else n_left++;
            } else {
                n_left++;
                hdf_log(&io, "%s: %s", hu->f.name, r == 0 ? "neither a Rigid Disk Block nor a filesystem at block 0 - ignored" : "unreadable RDB");
                hdf_file_free(&io, &hu->f);
                FreeMem(hu, sizeof *hu);
            }
        }
    }
    /* folders of the FAT partition as volumes (folders.c) */
    if (have_fat && eb) folders_mount(db, &io, eb, cd, config ? config : "");
    /* CD images as read-only volumes (isos.c) */
    if (ni && db->opts.iso_off) hdf_log(&io, "CD images turned off (iso=off)");
    else if (ni && eb) isos_mount(db, &io, eb, cd, isos, ni);
    for (int i = 0; i < ni; i++) hdf_file_free(&io, &isos[i]);      /* those not mounted */
    if (isos) FreeMem(isos, sizeof(struct hdf_file) * ISO_MAX);
    if (config) FreeMem(config, CONFIG_MAX);
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
    /* the kernel reads this line for the splash's status strip: keep its form */
    hdf_log(&io, "hardfiles: %ld bootable, %ld non-bootable, %ld left out", (LONG)n_boot, (LONG)n_other, (LONG)n_left);
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
