/* SPDX-License-Identifier: GPL-2.0-only
 * alice68hdf.device: what alice68hdf.c and folders.c share. */
#ifndef ALICE68_HDF_DEV_H
#define ALICE68_HDF_DEV_H
#include "../amiga.h"
#include "hdf_core.h"
#include "fatfs.h"
#include "exfat.h"

#define FOLDER_MAX   8                      /* folders mounted as volumes */
#define FOLDER_UNIT0 0x4000                 /* their FileSysStartupMsg units: not hardfile units */
struct FVol;
struct FObj;
struct SignalSemaphore { UWORD ss[23]; };   /* exec's 46 bytes, set up by InitSemaphore */
/* the start of a folder volume (folders.c's struct FVol), which Alice68-ShowConfig reads */
struct FVolHead { ULONG index; char path[128]; char vol[32]; char dev[8]; };

#define ISO_MAX      8                      /* CD images mounted as volumes */
#define ISO_UNIT0    0x5000                 /* their FileSysStartupMsg units */
struct IVol;
/* the start of a CD image volume (isos.c's struct IVol): Alice68-ShowConfig and folders.c read it */
struct IVolHead { ULONG index; char path[128]; char vol[32]; char dev[8]; struct hdf_file f; };

#define HDF_MAGIC  0x41363848u              /* 'A68H', read by Alice68-HDF */
#define LOG_SIZE   4096
#define TASK_STACK 8192
#define MAX_CHUNK  256u                     /* sectors per card request */
#define CONFIG_MAX 8192                     /* config.txt read, at most */

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
    /* volumes mounted, by name, so that none is there twice */
    struct { char vol[32], drive[32]; } vols[48];
    ULONG nvols;
    /* filesystems loaded from hardfiles' RDBs */
    struct { ULONG dostype, version, patchflags, dn[9]; } lfs[8];
    ULONG nlfs;
    /* folders on the FAT partition, as volumes (folders.c) */
    struct SignalSemaphore fsem;            /* one handler in the FAT engine at a time */
    struct fat ffat;
    struct ffs ffs;
    ULONG ffs_up;
    struct xfat xfat;                       /* the card's exFAT partition, for folders there */
    struct ffs xffs;                        /* the same calls on it (exfat.c) */
    ULONG xffs_up;
    struct FVol *fvol[FOLDER_MAX];
    ULONG nfvols;
    struct FObj *objs;                      /* every lock and open file, all volumes */
    ULONG mgen;                             /* +1 on every change a folder makes: the sidecar caches start again */
    APTR fseg;                              /* the handlers' seglist (RAM: a jump into this ROM) */
    /* CD images (*.iso) as read-only volumes (isos.c) */
    struct IVol *ivol[ISO_MAX];
    ULONG nivols;
    APTR iseg;                              /* their handlers' seglist */
    APTR ptb;                               /* Alice68's USB card (struct pt_hdr): Ctrl-Amiga-Amiga */
    struct Interrupt vbl;                   /* its vertical blank server */
};
ACHECK(AOFF(struct HdfBase, magic) == 36);
ACHECK(AOFF(struct HdfBase, unit) == 44);
ACHECK(AOFF(struct HdfBase, log) == 112);

/* the card, for hdf_core: one IORequest, used by one task at a time */
struct card { struct HdfBase *db; struct IOStdReq *io; };


void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
extern const char hdf_devname[];
void log_line(void *ctx, const char *line);
int  card_read(void *ctx, uint32_t sector, uint32_t count, void *buf);
int  card_write(void *ctx, uint32_t sector, uint32_t count, const void *buf);
void *core_alloc(void *ctx, uint32_t n);
void core_free(void *ctx, void *p, uint32_t n);
#define HDF_IO(c) { (c), card_read, card_write, core_alloc, core_free, log_line }
int  card_open(struct HdfBase *db, struct card *c, struct MsgPort **port);
void card_close(struct card *c, struct MsgPort *port);
int  str_ieq(const char *a, const char *b);
int  find_vol(struct HdfBase *db, const char *vol);
void add_vol(struct HdfBase *db, const char *vol, const char *drive);
struct BootNode *find_drive(APTR eb, const char *name);

/* folders.c */
void folders_mount(struct HdfBase *db, struct hdf_io *io, APTR eb, APTR cd, const char *config);
LONG highest_boot_pri(APTR eb);                 /* the mount list's highest boot priority (-128 if none) */
/* isos.c: takes over the files' extents */
void isos_mount(struct HdfBase *db, struct hdf_io *io, APTR eb, APTR cd, struct hdf_file *files, int n);
/* the RAM seglist dos.library starts a handler from: a call of entry(db) in the ROM */
APTR handler_seglist(struct HdfBase *db, void (*entry)(struct HdfBase *));
#endif
