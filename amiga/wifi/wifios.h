/* SPDX-License-Identifier: GPL-2.0-only
 * alice68wifi.device: the AmigaOS calls it needs, on top of amiga/amiga.h
 * (plain m68k ELF gcc, no NDK). lvo_x() in wifi_start.S loads d0-d3/a0/a1. */
#ifndef ALICE68_WIFIOS_H
#define ALICE68_WIFIOS_H
#include "../amiga.h"

#define EXEC_TYPES_H 1                  /* for netdev.h */
typedef void VOID;
typedef const char *CONST_STRPTR;
typedef char *STRPTR;

ULONG lvo_x(APTR base, LONG off, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG a0, ULONG a1);
#define LVO(b, o, d0, d1, d2, d3, a0, a1) lvo_x((APTR)(b), (o), (ULONG)(d0), (ULONG)(d1), (ULONG)(d2), (ULONG)(d3), (ULONG)(a0), (ULONG)(a1))

struct IOStdReq { struct IORequest io; ULONG io_Actual, io_Length; APTR io_Data; ULONG io_Offset; };
ACHECK(sizeof(struct IOStdReq) == 48);
struct timerequest { struct IORequest io; ULONG tv_secs, tv_micro; };
ACHECK(sizeof(struct timerequest) == 40);
struct NSDeviceQueryResult { ULONG DevQueryFormat, SizeAvailable; UWORD DeviceType, DeviceSubType; const UWORD *SupportedCommands; };

#define MEMF_PUBLIC   1u
#define MEMF_FAST     4u
#define MEMF_CLEAR    0x10000u
#define PA_SIGNAL     0
#define PA_IGNORE     2
#define NT_MSGPORT    4
#define NT_REPLYMSG   7
#define SIGBREAKF_CTRL_C (1u << 12)
#define TR_ADDREQUEST 9
#define MODE_OLDFILE  1005
#define OFFSET_BEGINNING (-1)
#define OFFSET_END    1
#define NP_Entry      (TAG_USER + 1003)
#define NP_StackSize  (TAG_USER + 1011)
#define NP_Name       (TAG_USER + 1012)
#define NP_Priority   (TAG_USER + 1013)

static inline APTR AllocMem(ULONG n, ULONG fl)            { return (APTR)lvo(SYSBASE, -198, n, fl, 0, 0); }
static inline APTR FindTask(const char *n)                { return (APTR)lvo(SYSBASE, -294, 0, 0, 0, (ULONG)n); }
static inline ULONG Wait(ULONG s)                         { return lvo(SYSBASE, -318, s, 0, 0, 0); }
static inline void Signal(APTR t, ULONG s)                { lvo(SYSBASE, -324, s, 0, 0, (ULONG)t); }
static inline LONG AllocSignal(LONG n)                    { return (LONG)lvo(SYSBASE, -330, (ULONG)n, 0, 0, 0); }
static inline void FreeSignal(LONG n)                     { lvo(SYSBASE, -336, (ULONG)n, 0, 0, 0); }
static inline void PutMsg(APTR p, APTR m)                 { lvo(SYSBASE, -366, 0, 0, (ULONG)p, (ULONG)m); }
static inline APTR GetMsg(APTR p)                         { return (APTR)lvo(SYSBASE, -372, 0, 0, (ULONG)p, 0); }
static inline void RawPutChar(char c)                     { lvo(SYSBASE, -516, (ULONG)(UBYTE)c, 0, 0, 0); }
static inline APTR OpenResource(const char *n)            { return (APTR)lvo(SYSBASE, -498, 0, 0, 0, (ULONG)n); }
static inline LONG DoIO(APTR io)                          { return (LONG)(BYTE)lvo(SYSBASE, -456, 0, 0, 0, (ULONG)io); }
static inline void SendIO(APTR io)                        { lvo(SYSBASE, -462, 0, 0, 0, (ULONG)io); }
static inline APTR CheckIO(APTR io)                       { return (APTR)lvo(SYSBASE, -468, 0, 0, 0, (ULONG)io); }
static inline void WaitIO(APTR io)                        { lvo(SYSBASE, -474, 0, 0, 0, (ULONG)io); }
static inline void AbortIO(APTR io)                       { lvo(SYSBASE, -480, 0, 0, 0, (ULONG)io); }
static inline APTR CreateIORequest(APTR port, ULONG n)    { return (APTR)lvo(SYSBASE, -654, n, 0, (ULONG)port, 0); }
static inline void DeleteIORequest(APTR io)               { lvo(SYSBASE, -660, 0, 0, (ULONG)io, 0); }
static inline void InitSemaphore(APTR s)                  { lvo(SYSBASE, -558, 0, 0, (ULONG)s, 0); }
static inline void ObtainSemaphore(APTR s)                { lvo(SYSBASE, -564, 0, 0, (ULONG)s, 0); }
static inline void ReleaseSemaphore(APTR s)               { lvo(SYSBASE, -570, 0, 0, (ULONG)s, 0); }
static inline APTR CreateMsgPort(void)                    { return (APTR)lvo(SYSBASE, -666, 0, 0, 0, 0); }
static inline void DeleteMsgPort(APTR p)                  { lvo(SYSBASE, -672, 0, 0, (ULONG)p, 0); }

/* dos.library */
static inline ULONG DosOpen(APTR dos, const char *n, LONG mode)    { return LVO(dos, -30, 0, n, mode, 0, 0, 0); }
static inline void DosClose(APTR dos, ULONG fh)                    { LVO(dos, -36, 0, fh, 0, 0, 0, 0); }
static inline LONG DosRead(APTR dos, ULONG fh, APTR b, LONG n)     { return (LONG)LVO(dos, -42, 0, fh, b, n, 0, 0); }
static inline LONG DosSeek(APTR dos, ULONG fh, LONG p, LONG mode)  { return (LONG)LVO(dos, -66, 0, fh, p, mode, 0, 0); }
static inline APTR CreateNewProc(APTR dos, const struct TagItem *t) { return (APTR)LVO(dos, -498, 0, t, 0, 0, 0, 0); }

/* gic400.library (in Emu68's ROM) */
static inline LONG AddIntServerEx(APTR gic, ULONG irq, ULONG pri, ULONG edge, struct Interrupt *i) { return (LONG)LVO(gic, -30, irq, pri, edge, 0, 0, i); }
static inline void RemIntServerEx(APTR gic, ULONG irq, struct Interrupt *i) { LVO(gic, -36, irq, 0, 0, 0, 0, i); }
static inline void DisableIntGic(APTR gic, ULONG irq)    { LVO(gic, -54, irq, 0, 0, 0, 0, 0); }

/* mailbox.resource (in Emu68's ROM) */
static inline ULONG MB_GetClockRate(APTR mb, ULONG id)              { return LVO(mb, -18, id, 0, 0, 0, 0, 0); }
static inline ULONG MB_SetClockState(APTR mb, ULONG id, ULONG on)   { return LVO(mb, -36, id, on, 0, 0, 0, 0); }
static inline ULONG MB_SetGPIOState(APTR mb, ULONG gpio, ULONG on)  { return LVO(mb, -60, gpio, on, 0, 0, 0, 0); }

static inline void NewList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = 0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}
#endif
