/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-Wheel - a USB mouse's scroll wheel on AmigaOS. The Amiga's mouse port has no wheel,
 * so the kernel counts the wheel's steps on Alice68's USB card (pt_hdr.wheel_y/_x, mouse_btn)
 * and this turns them into the NewMouse events programs know: raw keys $7A/$7B (wheel up and
 * down), $7C/$7D (left and right), $7E (the 4th button), each pressed and released, through
 * input.device. NEWMOUSE also sends them as IECLASS_NEWMOUSE, for tools that want that.
 * Runs in the background: from WBStartup (Install puts it there), or Run >NIL: Alice68-Wheel.
 * Started again (a Shell, or its icon), it stops the first one. */
#include "amiga.h"
#include "../third_party/Emu68/src/frf_usb/xhci/alice68_usbpt.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

struct IOStdReq { struct IORequest io; ULONG io_Actual, io_Length; APTR io_Data; ULONG io_Offset; };
struct InputEvent { APTR ie_NextEvent; UBYTE ie_Class, ie_SubClass; UWORD ie_Code, ie_Qualifier; WORD ie_X, ie_Y; ULONG ie_Secs, ie_Micro; };
ACHECK(sizeof(struct IOStdReq) == 48);
ACHECK(sizeof(struct InputEvent) == 22);
ACHECK(AOFF(struct pt_hdr, wheel_y) == 44 && AOFF(struct pt_hdr, mouse_btn) == 52 && sizeof(struct pt_hdr) == 192);

#define IND_WRITEEVENT     11
#define IECLASS_RAWKEY     0x01
#define IECLASS_NEWMOUSE   0x16
#define IECODE_UP_PREFIX   0x80
#define NM_WHEEL_UP        0x7a
#define NM_WHEEL_DOWN      0x7b
#define NM_WHEEL_LEFT      0x7c
#define NM_WHEEL_RIGHT     0x7d
#define NM_BUTTON_FOURTH   0x7e
#define SIGBREAKF_CTRL_C   (1u << 12)
#define PORTNAME           "Alice68-Wheel"
#define MAXSTEPS           8            /* per 1/50 s: a flick of a free-spinning wheel */

static inline APTR FindTask(const char *n)              { return (APTR)lvo(SYSBASE, -294, 0, 0, 0, (ULONG)n); }
static inline LONG SetTaskPri(APTR t, LONG p)          { return (LONG)(BYTE)lvo(SYSBASE, -300, (ULONG)p, 0, 0, (ULONG)t); }
static inline void Signal(APTR t, ULONG m)              { lvo(SYSBASE, -324, m, 0, 0, (ULONG)t); }
static inline void AddPort(struct MsgPort *p)           { lvo(SYSBASE, -354, 0, 0, 0, (ULONG)p); }
static inline void RemPort(struct MsgPort *p)           { lvo(SYSBASE, -360, 0, 0, 0, (ULONG)p); }
static inline struct MsgPort *FindPort(const char *n)   { return (struct MsgPort *)lvo(SYSBASE, -390, 0, 0, 0, (ULONG)n); }
static inline LONG DoIO(APTR io)                        { return (LONG)(BYTE)lvo(SYSBASE, -456, 0, 0, 0, (ULONG)io); }
static inline APTR CreateIORequest(struct MsgPort *p, ULONG n) { return (APTR)lvo(SYSBASE, -654, n, 0, (ULONG)p, 0); }
static inline void DeleteIORequest(APTR io)             { lvo(SYSBASE, -660, 0, 0, (ULONG)io, 0); }
static inline struct MsgPort *CreateMsgPort(void)       { return (struct MsgPort *)lvo(SYSBASE, -666, 0, 0, 0, 0); }
static inline void DeleteMsgPort(struct MsgPort *p)     { lvo(SYSBASE, -672, 0, 0, (ULONG)p, 0); }
static inline UWORD PeekQualifier(APTR input)           { return (UWORD)lvo(input, -42, 0, 0, 0, 0); }
static inline APTR GetMsg(struct MsgPort *p)            { return (APTR)lvo(SYSBASE, -372, 0, 0, (ULONG)p, 0); }
static inline void WaitPort(struct MsgPort *p)          { lvo(SYSBASE, -384, 0, 0, (ULONG)p, 0); }
#define PR_MSGPORT 92                                   /* struct Process: pr_MsgPort, pr_CLI */
#define PR_CLI     172

static APTR dos; static ULONG out;
static void puts_(const char *s) { LONG n = 0; while (s[n]) n++; if (out) Write(dos, out, s, n); }

static struct IOStdReq *io;
static int newmouse;

static void event(UBYTE cls, UWORD code, UWORD qual)
{
    struct InputEvent ie;
    memset(&ie, 0, sizeof ie);
    ie.ie_Class = cls; ie.ie_Code = code; ie.ie_Qualifier = qual;
    io->io.io_Command = IND_WRITEEVENT;
    io->io_Data = &ie;
    io->io_Length = sizeof ie;
    DoIO(io);
}
static void press(UWORD code, int down)
{
    UWORD qual = PeekQualifier(io->io.io_Device);           /* Shift-wheel etc. as the user holds them */
    if (!down) code |= IECODE_UP_PREFIX;
    if (newmouse) event(IECLASS_NEWMOUSE, code, qual);
    event(IECLASS_RAWKEY, code, qual);
}
static void steps(LONG n, UWORD plus, UWORD minus)
{
    if (n > MAXSTEPS) n = MAXSTEPS;
    if (n < -MAXSTEPS) n = -MAXSTEPS;
    for (; n > 0; n--) { press(plus, 1); press(plus, 0); }
    for (; n < 0; n++) { press(minus, 1); press(minus, 0); }
}

static int word_in(const char *a, ULONG n, const char *w)
{
    for (ULONG i = 0; i < n; i++) {
        ULONG k = 0;
        while (w[k] && i + k < n && (a[i + k] | 0x20) == (w[k] | 0x20)) k++;
        if (!w[k]) return 1;
    }
    return 0;
}

static LONG run(const char *args, ULONG arglen);

LONG wheel_main(const char *args, ULONG arglen)
{
    /* from Workbench (WBStartup, or the icon): take its message, give it back when done */
    UBYTE *me = FindTask(0);
    APTR wbmsg = 0;
    if (!*(ULONG *)(me + PR_CLI)) {
        WaitPort((struct MsgPort *)(me + PR_MSGPORT));
        wbmsg = GetMsg((struct MsgPort *)(me + PR_MSGPORT));
        arglen = 0;
    }
    LONG rc = run(args, arglen);
    if (wbmsg) { Forbid(); ReplyMsg(wbmsg); }
    return rc;
}

static LONG run(const char *args, ULONG arglen)
{
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = *(ULONG *)((UBYTE *)FindTask(0) + PR_CLI) ? Output(dos) : 0;     /* none from Workbench */
    newmouse = word_in(args, arglen, "NEWMOUSE");

    /* already running: stop that one */
    Forbid();
    struct MsgPort *other = FindPort(PORTNAME);
    if (other) Signal(other->mp_SigTask, SIGBREAKF_CTRL_C);
    Permit();
    if (other) { puts_("Alice68-Wheel: stopped\n"); CloseLibrary(dos); return 0; }

    APTR eb = OpenLibrary("expansion.library", 36);
    struct ConfigDev *cd = eb ? FindConfigDev(eb, 0, PT_MANUFACTURER, PT_PRODUCT) : 0;
    if (eb) CloseLibrary(eb);
    volatile struct pt_hdr *b = cd ? cd->cd_BoardAddr : 0;
    if (!b || b->magic != PT_MAGIC) {
        puts_("Alice68-Wheel: no Alice68 USB card (not an Alice68 kernel, or frf_usb=neptune)\n");
        CloseLibrary(dos);
        return 10;
    }

    struct MsgPort *pub = CreateMsgPort(), *rep = CreateMsgPort();
    io = rep ? CreateIORequest(rep, sizeof *io) : 0;
    if (!pub || !io || OpenDevice("input.device", 0, io, 0) != 0) {
        puts_("Alice68-Wheel: cannot open input.device\n");
        if (io) DeleteIORequest(io);
        if (rep) DeleteMsgPort(rep);
        if (pub) DeleteMsgPort(pub);
        CloseLibrary(dos);
        return 20;
    }
    pub->mp_Node.ln_Name = (char *)PORTNAME;
    AddPort(pub);
    APTR task = FindTask(0);
    LONG oldpri = SetTaskPri(task, 5);                        /* above busy programs at 0: no lag */
    puts_(newmouse ? "Alice68-Wheel: running, NewMouse events too (run it again to stop it)\n"
                   : "Alice68-Wheel: running (run it again to stop it)\n");

    LONG y = b->wheel_y, x = b->wheel_x;
    ULONG btn = b->mouse_btn;
    for (;;) {
        Delay(dos, 1);
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;
        LONG ny = b->wheel_y, nx = b->wheel_x;
        ULONG nb = b->mouse_btn;
        if (ny != y) { steps(ny - y, NM_WHEEL_UP, NM_WHEEL_DOWN); y = ny; }
        if (nx != x) { steps(nx - x, NM_WHEEL_RIGHT, NM_WHEEL_LEFT); x = nx; }
        if ((nb ^ btn) & 8) press(NM_BUTTON_FOURTH, (nb & 8) != 0);
        btn = nb;
    }

    if (btn & 8) press(NM_BUTTON_FOURTH, 0);
    SetTaskPri(task, oldpri);
    RemPort(pub);
    CloseDevice(io);
    DeleteIORequest(io);
    DeleteMsgPort(rep);
    DeleteMsgPort(pub);
    CloseLibrary(dos);
    return 0;
}
