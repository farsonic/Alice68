/* SPDX-License-Identifier: GPL-2.0-only
 * alice68hdf.device: dos.library's structures, packets and errors, for the DOS
 * handlers of the folders (folders.c) and the CD images (isos.c). */
#ifndef ALICE68_DOSDEFS_H
#define ALICE68_DOSDEFS_H
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
#define ERROR_COMMENT_TOO_BIG 220
#define FIBF_DELETE     1
#define FIBF_WRITE      4
#define FIBF_ARCHIVE    16

#define MEMF_PUBLIC 1
#define MEMF_CLEAR  0x10000

static __attribute__((unused)) APTR AllocMem(ULONG n, ULONG req)          { return (APTR)lvo(SYSBASE, -198, n, req, 0, 0); }
static __attribute__((unused)) APTR FindTask(const char *n)               { return (APTR)lvo(SYSBASE, -294, 0, 0, 0, (ULONG)n); }
static __attribute__((unused)) void PutMsg(struct MsgPort *p, APTR m)     { lvo(SYSBASE, -366, 0, 0, (ULONG)p, (ULONG)m); }
static __attribute__((unused)) APTR GetMsg(struct MsgPort *p)             { return (APTR)lvo(SYSBASE, -372, 0, 0, (ULONG)p, 0); }
static __attribute__((unused)) APTR WaitPort(struct MsgPort *p)           { return (APTR)lvo(SYSBASE, -384, 0, 0, (ULONG)p, 0); }
static __attribute__((unused)) void InitSemaphore(struct SignalSemaphore *s)    { lvo(SYSBASE, -558, 0, 0, (ULONG)s, 0); }
static __attribute__((unused)) void ObtainSemaphore(struct SignalSemaphore *s)  { lvo(SYSBASE, -564, 0, 0, (ULONG)s, 0); }
static __attribute__((unused)) void ReleaseSemaphore(struct SignalSemaphore *s) { lvo(SYSBASE, -570, 0, 0, (ULONG)s, 0); }
static __attribute__((unused)) void CacheClearU(void)                     { lvo(SYSBASE, -636, 0, 0, 0, 0); }
static __attribute__((unused)) APTR MakeDosNode(APTR eb, ULONG *pkt)      { return (APTR)lvo(eb, -144, 0, 0, (ULONG)pkt, 0); }
static __attribute__((unused)) LONG AddBootNode(APTR eb, LONG pri, ULONG fl, APTR dn, APTR cd) { return (LONG)lvo(eb, -36, (ULONG)pri, fl, (ULONG)dn, (ULONG)cd); }
static __attribute__((unused)) void DateStamp(APTR dos, struct DateStamp *d) { lvo_d1(dos, -192, (ULONG)d); }
static __attribute__((unused)) APTR LockDosList(APTR dos, ULONG f)        { return (APTR)lvo_d1(dos, -654, f); }
static __attribute__((unused)) void UnLockDosList(APTR dos, ULONG f)      { lvo_d1(dos, -660, f); }
static __attribute__((unused)) LONG AddDosEntry(APTR dos, struct DosList *d) { return (LONG)lvo_d1(dos, -678, (ULONG)d); }

#endif
