/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice68-usb.device - Neptune host-controller driver for Alice68
 * on a Raspberry Pi 4.
 *
 * The kernel drives the Pi 4's VL805 xHCI itself and keeps USB keyboards and
 * mice for the virtual Amiga. Every other device it finds - storage, network
 * adapters, ... - it offers to AmigaOS through a mailbox board (Zorro II,
 * 0x6d73/0x6a, see alice68_usbpt.h). This driver presents those devices to
 * Neptune (classic V2 IOUsbHWReq interface) as the ports of an emulated USB
 * 2.0 root hub, and forwards their transfers to the kernel, which carries
 * them out. Addressing is the xHC's: SET_ADDRESS is answered here.
 *
 * No task of its own: requests are posted from BeginIO, and the kernel's
 * PORTS (level 2) interrupt - with a vertical-blank poll behind it - replies
 * the finished ones.
 *
 *   USBStart alice68-usb.device 0     (or select it in Neptune USB)
 */
#include "amiga.h"
#include "../third_party/Emu68/src/frf_usb/xhci/alice68_usbpt.h"

#define VERSION   1
#define REVISION  0
#define DEVNAME   "alice68-usb.device"
#define IDSTRING  "alice68-usb.device 1.0 (05.11.2026) Alice68 Pi 4 USB pass-through"

/* Neptune's classic interface (devices/usbhardware.h, usbhcd_common.h) */
#define UHCMD_QUERYDEVICE   (CMD_NONSTD + 0)
#define UHCMD_USBRESET      (CMD_NONSTD + 1)
#define UHCMD_USBRESUME     (CMD_NONSTD + 2)
#define UHCMD_CONTROLXFER   (CMD_NONSTD + 3)
#define UHCMD_ISOXFER       (CMD_NONSTD + 4)
#define UHCMD_INTXFER       (CMD_NONSTD + 5)
#define UHCMD_BULKXFER      (CMD_NONSTD + 6)
#define UHDIR_IN            2
#define UHFF_LOWSPEED       1
#define UHIOERR_STALL       4
#define UHIOERR_TIMEOUT     6
#define UHIOERR_BADPARAMS   11
#define UHSF_OPERATIONAL    1
#define UHA_Dummy           (TAG_USER + 0x4711)
#define UHA_State           (UHA_Dummy + 0x01)
#define UHA_Manufacturer    (UHA_Dummy + 0x10)
#define UHA_ProductName     (UHA_Dummy + 0x11)
#define UHA_Version         (UHA_Dummy + 0x12)
#define UHA_Revision        (UHA_Dummy + 0x13)
#define UHA_Description     (UHA_Dummy + 0x14)
#define UHA_Copyright       (UHA_Dummy + 0x15)
#define UHA_DriverVersion   (UHA_Dummy + 0x20)
#define UHA_Capabilities    (UHA_Dummy + 0x21)
#define UHA_DMAAlignment    (UHA_Dummy + 0x25)
#define UHCF_USB20          1

#define NRH 8                                /* root hub ports = pass-through ports */
#define MAXWAIT 64
#define MAXRHINT 8

/* port status / change bits (USB 2.0 ch. 11) */
#define PS_CONN   0x0001
#define PS_ENABLE 0x0002
#define PS_RESET  0x0010
#define PS_POWER  0x0100
#define PS_LOW    0x0200
#define PS_HIGH   0x0400
#define PC_CONN   0x0001
#define PC_ENABLE 0x0002
#define PC_RESET  0x0010

struct DevBase {
    struct Library lib;
    /* offsets 34 and 38, read by Alice68-USBLog: why the last Open failed */
    ULONG diag;                              /* 0 ok, 1 no expansion.library, 2 no board, 3 bad magic, 4 bad version, 5 unit not 0 */
    ULONG diag_val;
    APTR seglist;
    struct Unit unit;
    volatile struct pt_hdr *b;
    struct Interrupt irq_ports, irq_vbl;
    int irqs_on;
    /* root hub */
    UWORD rh_addr, rh_addressed, rh_config;
    UWORD ps[NRH + 1], pc[NRH + 1];
    ULONG last_gen[NRH + 1];
    struct IOUsbHWReq *rh_int[MAXRHINT];
    /* devices */
    UBYTE addr_port[128];                    /* USB address -> pass-through port */
    UBYTE default_port;                      /* the port answering at address 0 */
    struct IOUsbHWReq *slot_io[PT_REQS];     /* in flight, by mailbox slot */
    struct IOUsbHWReq *waitq[MAXWAIT];       /* no free mailbox slot yet, in arrival order */
    int nwait;
    ULONG seq;                               /* submission order, for the kernel */
};

_Static_assert(__builtin_offsetof(struct DevBase, diag) == 34, "Alice68-USBLog reads diag at 34");

/* ---- small helpers -------------------------------------------------------- */
void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }
/* a 68k NOP is a full barrier under Emu68 (dsb sy): the kernel on another
   core must see a request's fields before its state, and we its results
   after its DONE */
static inline void barrier(void) { __asm volatile("nop" ::: "memory"); }
static inline UWORD le16(UWORD w) { return (UWORD)((w >> 8) | (w << 8)); }

static void alog(struct DevBase *db, const char *s, ULONG v)
{
    volatile struct pt_hdr *b = db->b;
    if (!b) return;
    char line[96]; int n = 0;
    const char *p = "[alice68-usb] ";
    while (*p && n < 80) line[n++] = *p++;
    while (*s && n < 80) line[n++] = *s++;
    if (v != 0xffffffffu) {
        line[n++] = ' '; line[n++] = '$';
        for (int i = 28; i >= 0; i -= 4) line[n++] = "0123456789abcdef"[(v >> i) & 15];
    }
    line[n++] = '\n';
    volatile char *ring = (volatile char *)((ULONG)b + PT_OFF_ALOG);
    ULONG w = b->alog_wr;
    for (int i = 0; i < n; i++) ring[(w + i) % PT_ALOG_BYTES] = line[i];
    b->alog_wr = w + n;
}

static void reply(struct IOUsbHWReq *io)
{
    if (!(io->iouh_Req.io_Flags & IOF_QUICK)) ReplyMsg(io);
}

/* ---- root hub ------------------------------------------------------------- */
static const UBYTE rh_dev_desc[18] = {
    18, 1, 0x00, 0x02, 9, 0, 1, 64, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x01, 1, 2, 0, 1
};
static const UBYTE rh_cfg_desc[25] = {
    9, 2, 25, 0, 1, 1, 0, 0xc0, 0,
    9, 4, 0, 0, 1, 9, 0, 0, 0,
    7, 5, 0x81, 3, 2, 0, 12
};
/* 8 ports: DeviceRemovable and PortPwrCtrlMask take 2 bytes each */
static const UBYTE rh_hub_desc[11] = { 11, 0x29, NRH, 0x09, 0x00, 1, 0, 0x00, 0x00, 0xff, 0xff };
static const UBYTE rh_str0[4] = { 4, 3, 0x09, 0x04 };

static int str_desc(const char *s, UBYTE *out, int max)
{
    int n = 2;
    while (*s && n + 2 <= max && n < 254) { out[n++] = (UBYTE)*s++; out[n++] = 0; }
    out[0] = (UBYTE)n; out[1] = 3;
    return n;
}

static UWORD rh_change_bitmap(struct DevBase *db)
{
    UWORD m = 0;
    for (int p = 1; p <= NRH; p++) if (db->pc[p]) m |= (UWORD)(1u << p);
    return m;
}

static void rh_int_check(struct DevBase *db)
{
    UWORD m = rh_change_bitmap(db);
    if (!m) return;
    for (int i = 0; i < MAXRHINT; i++) {
        struct IOUsbHWReq *io = db->rh_int[i];
        if (!io) continue;
        db->rh_int[i] = 0;
        UBYTE *d = io->iouh_Data;
        ULONG n = io->iouh_Length < 2 ? io->iouh_Length : 2;
        if (n > 0) d[0] = (UBYTE)m;
        if (n > 1) d[1] = (UBYTE)(m >> 8);
        io->iouh_Actual = n;
        io->iouh_Req.io_Error = 0;
        reply(io);
        return;                               /* one per change */
    }
}

/* the kernel's port table -> connection changes */
static void rh_poll_ports(struct DevBase *db)
{
    volatile struct pt_hdr *b = db->b;
    if (!b) return;
    for (int p = 1; p <= NRH; p++) {
        ULONG g = b->port[p - 1].gen;
        if (g == db->last_gen[p]) continue;
        db->last_gen[p] = g;
        barrier();                             /* the kernel writes present, then gen */
        UWORD old = db->ps[p];
        if (b->port[p - 1].present) {
            ULONG sp = b->port[p - 1].speed;
            db->ps[p] = (UWORD)((db->ps[p] & PS_POWER) | PS_CONN | (sp == 2 ? PS_LOW : (sp >= 3 ? PS_HIGH : 0)));
        } else {
            db->ps[p] = (UWORD)(db->ps[p] & PS_POWER);
            for (int a = 0; a < 128; a++) if (db->addr_port[a] == p) db->addr_port[a] = 0;
            if (db->default_port == p) db->default_port = 0;
        }
        db->pc[p] |= PC_CONN;
        (void)old;
        alog(db, b->port[p - 1].present ? "port connected" : "port disconnected", (ULONG)p);
    }
}

static int rh_control(struct DevBase *db, struct IOUsbHWReq *io)
{
    struct UsbSetupData *s = &io->iouh_SetupData;
    UWORD wv = le16(s->wValue), wi = le16(s->wIndex), wl = le16(s->wLength);
    UBYTE *out = io->iouh_Data;
    UBYTE tmp[64];
    const UBYTE *src = 0; int n = 0;
    UBYTE rt = s->bmRequestType, rq = s->bRequest;
    io->iouh_Actual = 0;

    if (rt == 0x00 && rq == 5) { db->rh_addr = wv; db->rh_addressed = 1; return 0; }        /* SET_ADDRESS */
    if (rt == 0x00 && rq == 9) { db->rh_config = wv; return 0; }                            /* SET_CONFIGURATION */
    if (rt == 0x80 && rq == 8) { tmp[0] = (UBYTE)db->rh_config; src = tmp; n = 1; }        /* GET_CONFIGURATION */
    else if (rt == 0x80 && rq == 0) { tmp[0] = 1; tmp[1] = 0; src = tmp; n = 2; }          /* GET_STATUS device: self powered */
    else if ((rt == 0x80 || rt == 0xa0) && rq == 6) {                                      /* GET_DESCRIPTOR */
        switch (wv >> 8) {
        case 1: src = rh_dev_desc; n = sizeof rh_dev_desc; break;
        case 2: src = rh_cfg_desc; n = sizeof rh_cfg_desc; break;
        case 0x29: src = rh_hub_desc; n = sizeof rh_hub_desc; break;
        case 3:
            if ((wv & 0xff) == 0) { src = rh_str0; n = 4; }
            else { n = str_desc((wv & 0xff) == 1 ? "Alice68" : "Pi 4 USB pass-through root hub", tmp, 64); src = tmp; }
            break;
        default: return UHIOERR_STALL;
        }
    }
    else if (rt == 0xa0 && rq == 0) { memset(tmp, 0, 4); src = tmp; n = 4; }              /* GetHubStatus */
    else if (rt == 0xa3 && rq == 0) {                                                      /* GetPortStatus */
        if (wi < 1 || wi > NRH) return UHIOERR_STALL;
        tmp[0] = (UBYTE)db->ps[wi]; tmp[1] = (UBYTE)(db->ps[wi] >> 8);
        tmp[2] = (UBYTE)db->pc[wi]; tmp[3] = (UBYTE)(db->pc[wi] >> 8);
        src = tmp; n = 4;
    }
    else if (rt == 0x20 && (rq == 1 || rq == 3)) return 0;                                 /* Clear/SetHubFeature */
    else if (rt == 0x23 && (rq == 1 || rq == 3)) {                                         /* Clear/SetPortFeature */
        if (wi < 1 || wi > NRH) return UHIOERR_STALL;
        int set = rq == 3;
        switch (wv) {
        case 1: if (!set) db->ps[wi] &= (UWORD)~PS_ENABLE; break;                          /* PORT_ENABLE */
        case 2:                                                                            /* PORT_SUSPEND */
            if (set) db->ps[wi] |= 0x0004;
            else if (db->ps[wi] & 0x0004) { db->ps[wi] &= (UWORD)~0x0004; db->pc[wi] |= 0x0004; }
            break;
        case 4:                                                                            /* PORT_RESET */
            if (set && (db->ps[wi] & PS_CONN)) {
                db->ps[wi] |= PS_ENABLE;
                db->pc[wi] |= PC_RESET;
                for (int a = 0; a < 128; a++) if (db->addr_port[a] == wi) db->addr_port[a] = 0;
                db->default_port = (UBYTE)wi;
                alog(db, "port reset, device at address 0 is port", wi);
            }
            break;
        case 8: if (set) db->ps[wi] |= PS_POWER; else db->ps[wi] &= (UWORD)~PS_POWER; break;   /* PORT_POWER */
        case 16: db->pc[wi] &= (UWORD)~PC_CONN; break;
        case 17: db->pc[wi] &= (UWORD)~PC_ENABLE; break;
        case 18: db->pc[wi] &= (UWORD)~0x0004; break;                                    /* C_PORT_SUSPEND */
        case 19: break;
        case 20: db->pc[wi] &= (UWORD)~PC_RESET; break;
        default: break;
        }
        rh_int_check(db);
        return 0;
    }
    else if (rt == 0x01 && rq == 11) return 0;                                             /* SET_INTERFACE */
    else return UHIOERR_STALL;

    if (n > wl) n = wl;
    if ((ULONG)n > io->iouh_Length) n = (int)io->iouh_Length;
    memcpy(out, src, (unsigned long)n);
    io->iouh_Actual = (ULONG)n;
    return 0;
}

/* ---- devices: through the mailbox ---------------------------------------- */
static int submit(struct DevBase *db, struct IOUsbHWReq *io, int port)
{
    volatile struct pt_hdr *b = db->b;
    for (int i = 0; i < PT_REQS; i++) {
        volatile struct pt_req *r = (volatile struct pt_req *)((ULONG)b + PT_OFF_REQS + (ULONG)i * 64u);
        if (r->state != PTS_FREE || db->slot_io[i]) continue;
        UWORD cmd = io->iouh_Req.io_Command;
        r->port = (ULONG)port;
        r->type = cmd == UHCMD_CONTROLXFER ? PTT_CONTROL : cmd == UHCMD_BULKXFER ? PTT_BULK : PTT_INT;
        r->ep = io->iouh_Endpoint;
        r->dir = io->iouh_Dir;
        r->len = cmd == UHCMD_CONTROLXFER ? le16(io->iouh_SetupData.wLength) : io->iouh_Length;
        if (cmd == UHCMD_CONTROLXFER && r->len > io->iouh_Length) r->len = io->iouh_Length;
        r->data = (ULONG)io->iouh_Data;
        const UBYTE *sp = (const UBYTE *)&io->iouh_SetupData;
        for (int k = 0; k < 8; k++) r->setup[k] = sp[k];
        if (cmd == UHCMD_CONTROLXFER) { r->setup[6] = (UBYTE)r->len; r->setup[7] = (UBYTE)(r->len >> 8); }
        r->flags = io->iouh_Flags;
        r->maxpkt = io->iouh_MaxPktSize;
        r->actual = 0; r->error = 0; r->cancel = 0;
        r->naktimeout = io->iouh_NakTimeout;
        r->seq = ++db->seq;
        db->slot_io[i] = io;
        io->iouh_DriverPrivate1 = (APTR)(ULONG)(i + 1);
        barrier();
        r->state = PTS_SUBMIT;
        return 1;
    }
    return 0;
}

static int dev_port(struct DevBase *db, struct IOUsbHWReq *io)
{
    UWORD a = io->iouh_DevAddr;
    int p = a == 0 ? db->default_port : (a < 128 ? db->addr_port[a] : 0);
    if (p < 1 || p > NRH || !(db->ps[p] & PS_CONN)) return 0;
    return p;
}

/* the interrupt's work, also called from BeginIO: everything under Disable */
static void service(struct DevBase *db)
{
    volatile struct pt_hdr *b = db->b;
    if (!b) return;
    int rounds = 0;
    do {
        b->irq = 0;
        barrier();
        rh_poll_ports(db);
        rh_int_check(db);
        for (int i = 0; i < PT_REQS; i++) {
            struct IOUsbHWReq *io = db->slot_io[i];
            if (!io) continue;
            volatile struct pt_req *r = (volatile struct pt_req *)((ULONG)b + PT_OFF_REQS + (ULONG)i * 64u);
            if (r->state != PTS_DONE) continue;
            barrier();
            io->iouh_Actual = r->actual;
            io->iouh_Req.io_Error = (BYTE)r->error;
            db->slot_io[i] = 0;
            io->iouh_DriverPrivate1 = 0;
            r->state = PTS_FREE;
            reply(io);
        }
        /* the waiting ones, oldest first; once one finds no free slot, the
           rest keep waiting behind it, in order */
        int n = 0, blocked = 0;
        for (int w = 0; w < db->nwait; w++) {
            struct IOUsbHWReq *io = db->waitq[w];
            int p = dev_port(db, io);
            if (!p) { io->iouh_Req.io_Error = UHIOERR_TIMEOUT; reply(io); continue; }
            if (!blocked && submit(db, io, p)) continue;
            blocked = 1;
            db->waitq[n++] = io;
        }
        db->nwait = n;
    } while (b->irq && ++rounds < 4);
}

/* ---- interrupt servers (stubs in start.S return with Z set) ---------------- */
void dev_irq(struct DevBase *db)
{
    if (!db->b) return;
    Disable();
    service(db);
    Enable();
}

/* ---- device vectors --------------------------------------------------------- */
static void find_board(struct DevBase *db)
{
    if (db->b) return;
    APTR eb = OpenLibrary("expansion.library", 36);
    if (!eb) { db->diag = 1; return; }
    struct ConfigDev *cd = FindConfigDev(eb, 0, PT_MANUFACTURER, PT_PRODUCT);
    CloseLibrary(eb);
    if (!cd) { db->diag = 2; return; }
    volatile struct pt_hdr *b = cd->cd_BoardAddr;
    if (b->magic != PT_MAGIC) { db->diag = 3; db->diag_val = b->magic; return; }
    if (b->version != PT_VERSION) { db->diag = 4; db->diag_val = b->version; return; }
    db->diag = 0;
    db->b = b;
    b->amiga_ver = (VERSION << 16) | REVISION;
    for (int p = 1; p <= NRH; p++) { db->ps[p] = PS_POWER; db->last_gen[p] = 0; }
    alog(db, "opened, board at", (ULONG)b);
}

extern void stub_irq(void);
LONG dev_open(struct DevBase *db, struct IOUsbHWReq *io, ULONG unit, ULONG flags)
{
    (void)flags;
    io->iouh_Req.io_Error = IOERR_OPENFAIL;
    if (unit != 0) { db->diag = 5; db->diag_val = unit; return -1; }
    find_board(db);
    if (!db->b) return -1;
    if (!db->irqs_on) {
        db->irq_ports.is_Node.ln_Type = 2;             /* NT_INTERRUPT */
        db->irq_ports.is_Node.ln_Pri = 0;
        db->irq_ports.is_Node.ln_Name = (char *)DEVNAME;
        db->irq_ports.is_Data = db;
        db->irq_ports.is_Code = stub_irq;
        db->irq_vbl = db->irq_ports;
        db->irq_vbl.is_Node.ln_Pri = -10;
        AddIntServer(INTB_PORTS, &db->irq_ports);
        AddIntServer(INTB_VERTB, &db->irq_vbl);
        db->irqs_on = 1;
    }
    io->iouh_Req.io_Unit = &db->unit;
    io->iouh_Req.io_Error = 0;
    db->unit.unit_OpenCnt++;
    db->lib.lib_OpenCnt++;
    db->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return 0;
}

static ULONG do_expunge(struct DevBase *db)
{
    if (db->lib.lib_OpenCnt) { db->lib.lib_Flags |= LIBF_DELEXP; return 0; }
    if (db->irqs_on) { RemIntServer(INTB_PORTS, &db->irq_ports); RemIntServer(INTB_VERTB, &db->irq_vbl); db->irqs_on = 0; }
    ULONG seg = (ULONG)db->seglist;
    Remove(&db->lib.lib_Node);
    FreeMem((UBYTE *)db - db->lib.lib_NegSize, (ULONG)db->lib.lib_NegSize + db->lib.lib_PosSize);
    return seg;
}

ULONG dev_close(struct DevBase *db, struct IOUsbHWReq *io)
{
    io->iouh_Req.io_Unit = (struct Unit *)-1;
    io->iouh_Req.io_Device = (APTR)-1;
    if (db->unit.unit_OpenCnt) db->unit.unit_OpenCnt--;
    if (db->lib.lib_OpenCnt) db->lib.lib_OpenCnt--;
    if (!db->lib.lib_OpenCnt && (db->lib.lib_Flags & LIBF_DELEXP)) return do_expunge(db);
    return 0;
}

ULONG dev_expunge(struct DevBase *db) { return do_expunge(db); }

static void query(struct DevBase *db, struct IOUsbHWReq *io)
{
    struct TagItem *t = io->iouh_Data;
    ULONG n = 0;
    while (t) {
        ULONG tag = t->ti_Tag, v; int known = 1;
        if (tag == TAG_DONE) break;
        if (tag == TAG_MORE) { t = (struct TagItem *)t->ti_Data; continue; }
        if (tag == TAG_SKIP) { t += 1 + t->ti_Data; continue; }
        switch (tag) {
        case UHA_State:         v = UHSF_OPERATIONAL; io->iouh_State = UHSF_OPERATIONAL; break;
        case UHA_Manufacturer:  v = (ULONG)"Alice68"; break;
        case UHA_ProductName:   v = (ULONG)"Pi 4 USB pass-through"; break;
        case UHA_Version:       v = VERSION; break;
        case UHA_Revision:      v = REVISION; break;
        case UHA_Description:   v = (ULONG)"USB devices of the Alice68 kernel's xHCI (VL805), keyboards/mice excepted"; break;
        case UHA_Copyright:     v = (ULONG)"GPL-2.0, Alice68"; break;
        case UHA_DriverVersion: v = 0x200; break;
        case UHA_Capabilities:  v = UHCF_USB20; break;
        case UHA_DMAAlignment:  v = 4; break;
        default: known = 0; v = 0; break;
        }
        if (known && tag != TAG_IGNORE) { *(ULONG *)t->ti_Data = v; n++; }
        t++;
    }
    io->iouh_Actual = n;
    (void)db;
}

void dev_beginio(struct DevBase *db, struct IOUsbHWReq *io)
{
    io->iouh_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->iouh_Req.io_Error = 0;
    UWORD cmd = io->iouh_Req.io_Command;
    int err = 0, deferred = 0;
    switch (cmd) {
    case CMD_RESET: case CMD_START: case CMD_STOP: case UHCMD_USBRESUME:
        break;
    case CMD_FLUSH:
        Disable();
        for (int i = 0; i < MAXRHINT; i++) if (db->rh_int[i]) { struct IOUsbHWReq *r = db->rh_int[i]; db->rh_int[i] = 0; r->iouh_Req.io_Error = IOERR_ABORTED; reply(r); }
        for (int w = 0; w < db->nwait; w++) { struct IOUsbHWReq *r = db->waitq[w]; r->iouh_Req.io_Error = IOERR_ABORTED; reply(r); }
        db->nwait = 0;
        for (int i = 0; i < PT_REQS; i++) if (db->slot_io[i]) ((volatile struct pt_req *)((ULONG)db->b + PT_OFF_REQS + (ULONG)i * 64u))->cancel = 1;
        Enable();
        break;
    case UHCMD_QUERYDEVICE:
        query(db, io);
        break;
    case UHCMD_USBRESET:
        Disable();
        db->rh_addr = 0; db->rh_addressed = 0; db->rh_config = 0;
        for (int a = 0; a < 128; a++) db->addr_port[a] = 0;
        db->default_port = 0;
        for (int p = 1; p <= NRH; p++) { db->ps[p] &= (UWORD)~PS_ENABLE; db->last_gen[p] = 0; db->pc[p] = 0; }
        rh_poll_ports(db);
        Enable();
        io->iouh_State = UHSF_OPERATIONAL;
        alog(db, "bus reset", 0xffffffffu);
        break;
    case UHCMD_CONTROLXFER: case UHCMD_INTXFER: case UHCMD_BULKXFER: {
        int to_rh = io->iouh_DevAddr == db->rh_addr && (db->rh_addressed ? io->iouh_DevAddr != 0 : 1);
        if (to_rh) {
            if (cmd == UHCMD_CONTROLXFER) { Disable(); err = rh_control(db, io); Enable(); break; }
            if (cmd == UHCMD_INTXFER) {
                Disable();
                if (rh_change_bitmap(db)) {
                    int q = 0;
                    for (int i = 0; i < MAXRHINT; i++) if (!db->rh_int[i]) { db->rh_int[i] = io; q = 1; break; }
                    if (!q) { Enable(); err = UHIOERR_BADPARAMS; break; }
                    io->iouh_Req.io_Flags &= (UBYTE)~IOF_QUICK;
                    rh_int_check(db);
                    Enable();
                    return;
                }
                for (int i = 0; i < MAXRHINT; i++) if (!db->rh_int[i]) { db->rh_int[i] = io; deferred = 1; break; }
                Enable();
                if (!deferred) err = UHIOERR_BADPARAMS;
                break;
            }
            err = UHIOERR_STALL;
            break;
        }
        Disable();
        int p = dev_port(db, io);
        if (!p) { Enable(); err = UHIOERR_TIMEOUT; break; }
        if (cmd == UHCMD_CONTROLXFER && io->iouh_SetupData.bmRequestType == 0x00 && io->iouh_SetupData.bRequest == 5) {
            UWORD na = le16(io->iouh_SetupData.wValue);                                /* SET_ADDRESS: the xHC addressed it */
            if (na < 128) db->addr_port[na] = (UBYTE)p;
            if (db->default_port == p) db->default_port = 0;
            Enable();
            alog(db, "address assigned for port", (ULONG)(p << 8 | na));
            io->iouh_Actual = 0;
            break;
        }
        io->iouh_Req.io_Flags &= (UBYTE)~IOF_QUICK;
        if (db->nwait || !submit(db, io, p)) {          /* behind any already waiting: keep the order */
            if (db->nwait >= MAXWAIT) { Enable(); io->iouh_Req.io_Error = 12 /* OUTOFMEMORY */; ReplyMsg(io); return; }
            db->waitq[db->nwait++] = io;
        }
        Enable();
        return;
    }
    case UHCMD_ISOXFER:
        err = UHIOERR_BADPARAMS;
        break;
    default:
        err = IOERR_NOCMD;
        break;
    }
    if (deferred) { io->iouh_Req.io_Flags &= (UBYTE)~IOF_QUICK; return; }
    io->iouh_Req.io_Error = (BYTE)err;
    reply(io);
}

LONG dev_abortio(struct DevBase *db, struct IOUsbHWReq *io)
{
    Disable();
    for (int i = 0; i < MAXRHINT; i++)
        if (db->rh_int[i] == io) { db->rh_int[i] = 0; io->iouh_Req.io_Error = IOERR_ABORTED; ReplyMsg(io); Enable(); return 0; }
    for (int w = 0; w < db->nwait; w++)
        if (db->waitq[w] == io) {
            for (int k = w; k + 1 < db->nwait; k++) db->waitq[k] = db->waitq[k + 1];
            db->nwait--;
            io->iouh_Req.io_Error = IOERR_ABORTED; ReplyMsg(io); Enable(); return 0;
        }
    ULONG s = (ULONG)io->iouh_DriverPrivate1;
    if (s >= 1 && s <= PT_REQS && db->slot_io[s - 1] == io)
        { barrier(); ((volatile struct pt_req *)((ULONG)db->b + PT_OFF_REQS + (s - 1) * 64u))->cancel = 1; }   /* the kernel replies ABORTED */
    Enable();
    return 0;
}

ULONG dev_init(struct DevBase *db, APTR seglist)
{
    db->seglist = seglist;
    db->lib.lib_Node.ln_Type = NT_DEVICE;
    db->lib.lib_Node.ln_Name = (char *)DEVNAME;
    db->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    db->lib.lib_Version = VERSION;
    db->lib.lib_Revision = REVISION;
    db->lib.lib_IdString = (APTR)IDSTRING;
    db->unit.unit_MsgPort.mp_Node.ln_Type = 4;   /* NT_MSGPORT */
    db->unit.unit_MsgPort.mp_Flags = 2;          /* PA_IGNORE */
    return (ULONG)db;
}

const char dev_name[] = DEVNAME;
const char dev_idstring[] = IDSTRING;
/* the auto-init table the ROMTag points at (device_start.S) */
extern void stub_init(void), stub_open(void), stub_close(void), stub_expunge(void),
            stub_null(void), stub_beginio(void), stub_abortio(void);
const ULONG dev_functab[] = {
    (ULONG)stub_open, (ULONG)stub_close, (ULONG)stub_expunge, (ULONG)stub_null,
    (ULONG)stub_beginio, (ULONG)stub_abortio, (ULONG)-1
};
const ULONG dev_inittab[4] = { sizeof(struct DevBase), (ULONG)dev_functab, 0, (ULONG)stub_init };
