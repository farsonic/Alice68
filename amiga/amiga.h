/* SPDX-License-Identifier: GPL-2.0-only
 * Minimal AmigaOS definitions for Alice68's 68k tools, built with a plain
 * m68k ELF gcc (no NDK needed): the few exec/expansion/dos structures and
 * library calls used, with their offsets checked at compile time. */
#ifndef ALICE68_AMIGA_H
#define ALICE68_AMIGA_H

typedef unsigned char  UBYTE;
typedef signed char    BYTE;
typedef unsigned short UWORD;
typedef short          WORD;
typedef unsigned int   ULONG;
typedef int            LONG;
typedef void          *APTR;

#define AOFF(t, f) __builtin_offsetof(t, f)
#define ACHECK(c) _Static_assert(c, #c)

struct Node { struct Node *ln_Succ, *ln_Pred; UBYTE ln_Type; BYTE ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; UBYTE lh_Type, l_pad; };
struct MsgPort { struct Node mp_Node; UBYTE mp_Flags, mp_SigBit; APTR mp_SigTask; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; UWORD mn_Length; };
struct Library { struct Node lib_Node; UBYTE lib_Flags, lib_pad; UWORD lib_NegSize, lib_PosSize, lib_Version, lib_Revision;
                 APTR lib_IdString; ULONG lib_Sum; UWORD lib_OpenCnt; };
struct Unit { struct MsgPort unit_MsgPort; UBYTE unit_flags, unit_pad; UWORD unit_OpenCnt; };
struct IORequest { struct Message io_Message; APTR io_Device; struct Unit *io_Unit; UWORD io_Command; UBYTE io_Flags; BYTE io_Error; };
struct Interrupt { struct Node is_Node; APTR is_Data; void (*is_Code)(void); };
struct TagItem { ULONG ti_Tag, ti_Data; };
struct ConfigDev { struct Node cd_Node; UBYTE cd_Flags, cd_Pad; UBYTE cd_Rom[16]; APTR cd_BoardAddr; ULONG cd_BoardSize; };

ACHECK(sizeof(struct Node) == 14);
ACHECK(sizeof(struct MsgPort) == 34);
ACHECK(sizeof(struct Message) == 20);
ACHECK(sizeof(struct Library) == 34);
ACHECK(sizeof(struct Unit) == 38);
ACHECK(sizeof(struct IORequest) == 32);
ACHECK(sizeof(struct Interrupt) == 22);
ACHECK(AOFF(struct ConfigDev, cd_BoardAddr) == 32);

struct UsbSetupData { UBYTE bmRequestType, bRequest; UWORD wValue, wIndex, wLength; };   /* words little-endian */
struct IOUsbHWReq {
    struct IORequest iouh_Req;
    UWORD iouh_Flags, iouh_State, iouh_Dir, iouh_DevAddr, iouh_Endpoint, iouh_MaxPktSize;
    ULONG iouh_Actual, iouh_Length;
    APTR  iouh_Data;
    UWORD iouh_Interval;
    ULONG iouh_NakTimeout;
    struct UsbSetupData iouh_SetupData;
    APTR  iouh_UserData;
    UWORD iouh_ExtError;
    UWORD iouh_Frame, iouh_SplitHubAddr, iouh_SplitHubPort;
    APTR  iouh_DriverPrivate1, iouh_DriverPrivate2;
};
ACHECK(AOFF(struct IOUsbHWReq, iouh_Actual) == 44);
ACHECK(AOFF(struct IOUsbHWReq, iouh_SetupData) == 62);
ACHECK(AOFF(struct IOUsbHWReq, iouh_ExtError) == 74);
ACHECK(sizeof(struct IOUsbHWReq) == 90);

#define NT_DEVICE   3
#define NT_MESSAGE  5
#define LIBF_CHANGED 2
#define LIBF_SUMUSED 4
#define LIBF_DELEXP  8
#define IOF_QUICK    1
#define IOERR_OPENFAIL (-1)
#define IOERR_ABORTED  (-2)
#define IOERR_NOCMD    (-3)
#define CMD_RESET   1
#define CMD_STOP    6
#define CMD_START   7
#define CMD_FLUSH   8
#define CMD_NONSTD  9
#define NSCMD_DEVICEQUERY 0x4000
#define INTB_PORTS  3
#define INTB_VERTB  5
#define TAG_DONE    0
#define TAG_IGNORE  1
#define TAG_MORE    2
#define TAG_SKIP    3
#define TAG_USER    0x80000000u

/* ---- library calls (lvo.S): d0/d1/a0/a1 in, d0 out, a6 = base ------------------ */
ULONG lvo_call(APTR base, LONG off, ULONG d0, ULONG d1, ULONG a0, ULONG a1);
LONG  dos_write(APTR dos, ULONG fh, const void *buf, LONG len);
ULONG lvo_d1(APTR base, LONG off, ULONG d1);
#define lvo(base, off, d0, d1, a0, a1) lvo_call((base), (off), (ULONG)(d0), (ULONG)(d1), (ULONG)(a0), (ULONG)(a1))
static inline __attribute__((always_inline)) APTR sysbase_(void) { APTR p; __asm volatile("move.l 4.w,%0" : "=a"(p)); return p; }
#define SYSBASE sysbase_()
static inline void Disable(void)                          { lvo(SYSBASE, -120, 0, 0, 0, 0); }
static inline void Enable(void)                           { lvo(SYSBASE, -126, 0, 0, 0, 0); }
static inline void AddIntServer(ULONG n, struct Interrupt *i) { lvo(SYSBASE, -168, n, 0, 0, (ULONG)i); }
static inline void RemIntServer(ULONG n, struct Interrupt *i) { lvo(SYSBASE, -174, n, 0, 0, (ULONG)i); }
static inline void FreeMem(APTR p, ULONG n)              { lvo(SYSBASE, -210, n, 0, 0, (ULONG)p); }
static inline void Remove(struct Node *n)                 { lvo(SYSBASE, -252, 0, 0, 0, (ULONG)n); }
static inline void ReplyMsg(APTR m)                       { lvo(SYSBASE, -378, 0, 0, 0, (ULONG)m); }
static inline void CloseLibrary(APTR l)                   { lvo(SYSBASE, -414, 0, 0, 0, (ULONG)l); }
static inline APTR OpenLibrary(const char *n, ULONG v)    { return (APTR)lvo(SYSBASE, -552, v, 0, 0, (ULONG)n); }
static inline struct ConfigDev *FindConfigDev(APTR eb, struct ConfigDev *old, LONG man, LONG prod)
{ return (struct ConfigDev *)lvo(eb, -72, (ULONG)man, (ULONG)prod, (ULONG)old, 0); }
static inline void Forbid(void)                           { lvo(SYSBASE, -132, 0, 0, 0, 0); }
static inline void Permit(void)                           { lvo(SYSBASE, -138, 0, 0, 0, 0); }
static inline APTR FindName(APTR list, const char *n)     { return (APTR)lvo(SYSBASE, -276, 0, 0, (ULONG)list, (ULONG)n); }
static inline LONG OpenDevice(const char *n, ULONG unit, APTR io, ULONG fl) { return (LONG)(BYTE)lvo(SYSBASE, -444, unit, fl, (ULONG)n, (ULONG)io); }
static inline void CloseDevice(APTR io)                   { lvo(SYSBASE, -450, 0, 0, 0, (ULONG)io); }
#define EXEC_DEVICELIST 350                               /* offset of DeviceList in ExecBase */
static inline ULONG SetSignal(ULONG nw, ULONG mask)       { return lvo(SYSBASE, -306, nw, mask, 0, 0); }
static inline void Delay(APTR dos, LONG ticks)           { lvo_d1(dos, -198, (ULONG)ticks); }
static inline ULONG Output(APTR dos)                      { return lvo(dos, -60, 0, 0, 0, 0); }
static inline LONG Write(APTR dos, ULONG fh, const void *buf, LONG len) { return dos_write(dos, fh, buf, len); }

/* freestanding helpers gcc may call */
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
#endif
