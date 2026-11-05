/* agaboot - the PiStorm USB's mouse and pad, found through the USB stack
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * What agaboot USBMOUSE / USBPAD are told by hand (usbmouse.c) - which
 * device, which endpoint, and where in its reports the buttons and the rest
 * are - found here instead: the devices Poseidon has, each HID interface's
 * interrupt IN endpoint, and that interface's report descriptor, asked for
 * on the device's control pipe the way Poseidon's hid.class asks for it.
 * Only this file knows Poseidon; another USB stack, when one comes, gets its
 * own usbf_find. I'm not building a HAL for a stack that doesn't exist yet.
 *
 * The kernel (arm/aga_usb.c) finds the device on the xHCI by where it is
 * plugged in: the root port, and the route string of hub ports below it
 * (xHCI 4.3.3). The address Poseidon gives a device is its own, not the
 * controller's - the CM4's RumblePad: Poseidon 3, the xHCI 2 (31.10.2026).
 * The route is built as rondoval's driver builds it (xhci-context.c,
 * build_route_string): the port on the first hub below the root in bits
 * 3-0, the next hub's in 7-4. Get that shift wrong and you're talking to
 * somebody else's mouse.
 *
 * Poseidon is asked only when it is in memory already - never loaded from
 * here, at a boot before its stack is up. Its read lock is held across the
 * scan, the control pipes included, as its own PsdDevLister does.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/macros.h>
#include <string.h>
#include <stdio.h>
#include "usbfind.h"

/* poseidon.library 4, as AROS's poseidon.conf lists it - the offsets match
   the calls in the user's Poseidon 4.5 PsdDevLister. If they ever change,
   this whole file is a pile of rubble. */
static struct Library *PsdBase;
#define psdLockReadPBase()             LP0NR(48, psdLockReadPBase, , PsdBase)
#define psdUnlockPBase()               LP0NR(54, psdUnlockPBase, , PsdBase)
#define psdGetNextDevice(pd)           LP1(126, APTR, psdGetNextDevice, APTR, pd, a0, , PsdBase)
#define psdGetAttrsA(ty, s, t)         LP3(132, LONG, psdGetAttrsA, ULONG, ty, d0, APTR, s, a0, struct TagItem *, t, a1, , PsdBase)
#define psdSetAttrsA(ty, s, t)         LP3(138, LONG, psdSetAttrsA, ULONG, ty, d0, APTR, s, a0, struct TagItem *, t, a1, , PsdBase)
#define psdAllocPipe(pd, mp, pep)      LP3(144, APTR, psdAllocPipe, APTR, pd, a0, struct MsgPort *, mp, a1, APTR, pep, a2, , PsdBase)
#define psdFreePipe(pp)                LP1(150, APTR, psdFreePipe, APTR, pp, a1, , PsdBase)
#define psdPipeSetup(pp, rt, rq, v, i) LP5NR(156, psdPipeSetup, APTR, pp, a1, ULONG, rt, d0, ULONG, rq, d1, ULONG, v, d2, ULONG, i, d3, , PsdBase)
#define psdDoPipe(pp, d, l)            LP3(162, LONG, psdDoPipe, APTR, pp, a1, APTR, d, a0, ULONG, l, d0, , PsdBase)
#define psdGetPipeActual(pp)           LP1(186, ULONG, psdGetPipeActual, APTR, pp, a1, , PsdBase)

#define PGA_HARDWARE         3
#define PGA_DEVICE           4
#define PGA_CONFIG           5
#define PGA_INTERFACE        6
#define PGA_ENDPOINT         7
#define PGA_PIPE             9
#define DA_Dummy             (TAG_USER + 42)
#define DA_Address           (DA_Dummy + 0x10)
#define DA_CurrConfig        (DA_Dummy + 0x12)
#define DA_HubDevice         (DA_Dummy + 0x13)
#define DA_ProductName       (DA_Dummy + 0x1c)
#define DA_Hardware          (DA_Dummy + 0x1e)
#define DA_ConfigList        (DA_Dummy + 0x20)
#define DA_AtHubPortNumber   (DA_Dummy + 0x26)
#define HA_Dummy             (TAG_USER + 0x2612)
#define HA_DeviceName        (HA_Dummy + 0x10)
#define HA_DeviceUnit        (HA_Dummy + 0x11)
#define CA_Dummy             (TAG_USER + 23)
#define CA_ConfigNum         (CA_Dummy + 0x10)
#define CA_InterfaceList     (CA_Dummy + 0x20)
#define IFA_Dummy            (TAG_USER + 4711)
#define IFA_InterfaceNum     (IFA_Dummy + 0x10)
#define IFA_Class            (IFA_Dummy + 0x12)
#define IFA_EndpointList     (IFA_Dummy + 0x20)
#define EA_Dummy             (TAG_USER + 1138)
#define EA_IsIn              (EA_Dummy + 0x01)
#define EA_EndpointNum       (EA_Dummy + 0x10)
#define EA_TransferType      (EA_Dummy + 0x11)
#define PPA_Dummy            (TAG_USER + 1234)
#define PPA_NakTimeout       (PPA_Dummy + 0x08)
#define PPA_NakTimeoutTime   (PPA_Dummy + 0x09)

#define USEAF_INTERRUPT      3
#define HID_CLASSCODE        3
#define URTF_IN              0x80
#define URTF_INTERFACE       0x01
#define USR_GET_DESCRIPTOR   0x06
#define UDT_HID              0x21
#define UDT_REPORT           0x22
#define UHIOERR_OVERFLOW     7
#define UHIOERR_RUNTPACKET   9

/* one attribute of a Poseidon structure; 0 if the bugger has none */
static ULONG attr(ULONG type, APTR s, ULONG tag)
{
    ULONG v = 0;
    struct TagItem t[2] = { { tag, (ULONG)&v }, { TAG_END, 0 } };
    psdGetAttrsA(type, s, t);
    return v;
}

/* ---- the report descriptor (HID 1.11, 6.2.2) ------------------------------ */

#define MAXF 96
#define GD(u)     (0x00010000ul | (u))             /* Generic Desktop, the HID page I care about */
#define BUTTON(n) (0x00090000ul | (n))

struct field {
    ULONG app, usage;                              /* page << 16 | usage: its application collection's, its own, packed tight */
    UWORD off;
    UBYTE id, bits, rel;
    LONG min, max;
};

struct globals { ULONG page, rsize, rcount, lmax_u; LONG lmin, lmax; UBYTE rid; };

/* the input reports' data fields, each variable one with its usage; arrays
   (a keyboard's keys) and constants only move the offset on. I don't care
   about your fancy keyboards. */
static int parse(const UBYTE *d, int len, struct field *f)
{
    struct globals g = { 0 }, st[4];
    ULONG use[16], umin = 0, umax = 0, app = 0;
    UBYTE ids[16];
    UWORD offs[16];
    int nid = 0, nu = 0, range = 0, depth = 0, nf = 0, sp = 0;
    for (int i = 0; i < len;) {
        UBYTE p = d[i++];
        if (p == 0xFE) {                           /* a long item: its size, its tag, its data - skip the lot */
            if (i >= len) break;
            i += 2 + d[i];
            continue;
        }
        int sz = (p & 3) == 3 ? 4 : (p & 3);
        if (i + sz > len) break;
        ULONG v = 0;
        for (int k = 0; k < sz; k++) v |= (ULONG)d[i + k] << (8 * k);
        LONG sv = sz == 1 ? (BYTE)v : sz == 2 ? (WORD)v : (LONG)v;
        i += sz;
        int type = (p >> 2) & 3, tag = p >> 4;
        if (type == 1) {                           /* global: the state every field inherits */
            if (tag == 0) g.page = v;
            else if (tag == 1) g.lmin = sv;
            else if (tag == 2) { g.lmax = sv; g.lmax_u = v; }
            else if (tag == 7) g.rsize = v;
            else if (tag == 8) g.rid = (UBYTE)v;
            else if (tag == 9) g.rcount = v;
            else if (tag == 10 && sp < 4) st[sp++] = g;
            else if (tag == 11 && sp > 0) g = st[--sp];
        } else if (type == 2) {                    /* local: they die with the next main item */
            ULONG u = sz == 4 ? v : g.page << 16 | v;
            if (tag == 0 && nu < 16) use[nu++] = u;
            else if (tag == 1) { umin = u; range |= 1; }
            else if (tag == 2) { umax = u; range |= 2; }
        } else if (type == 0) {                    /* main: the actual data */
            if (tag == 10) {                       /* a collection: the outermost one's usage is the application's */
                if (!depth++) app = nu ? use[0] : (range & 1) ? umin : 0;
            } else if (tag == 12) {
                if (depth) depth--;
            } else if (tag == 8) {                 /* input: the one that matters */
                int j = 0;
                while (j < nid && ids[j] != g.rid) j++;
                if (j == nid) {
                    if (nid == 16) return nf;
                    ids[nid] = g.rid;
                    offs[nid++] = g.rid ? 8 : 0;   /* the ID byte first, don't forget it */
                }
                LONG mx = g.lmax < g.lmin ? (LONG)g.lmax_u : g.lmax;   /* an unsigned maximum written short (0xFF for 255) */
                for (ULONG k = 0; k < g.rcount && k < 256; k++) {
                    ULONG u = range == 3 ? (umin + k <= umax ? umin + k : umax)
                            : nu ? use[k < (ULONG)nu ? k : (ULONG)nu - 1] : 0;
                    if (!(v & 1) && (v & 2) && u && nf < MAXF && g.rsize && g.rsize <= 32) {
                        f[nf].app = app; f[nf].usage = u; f[nf].off = offs[j]; f[nf].id = g.rid;
                        f[nf].bits = (UBYTE)g.rsize; f[nf].rel = (v & 4) != 0;
                        f[nf].min = g.lmin; f[nf].max = mx;
                        nf++;
                    }
                    offs[j] += (UWORD)g.rsize;
                }
            }
            nu = 0;                                /* locals end with each main item, every damn time */
            range = 0;
        }
    }
    return nf;
}

static const struct field *pick(const struct field *f, int nf, ULONG usage, int id)
{
    for (int i = 0; i < nf; i++)
        if (f[i].usage == usage && f[i].id == id) return &f[i];
    return 0;
}

/* kind 0 a mouse (moving X and Y), 1 a pad (a joystick's or game pad's hat
   or stick): the report that has it, laid out; 0 if the fields are none */
static int layout(const struct field *f, int nf, int kind, struct usbf *o)
{
    int id = -1;
    for (int i = 0; i < nf && id < 0; i++) {
        ULONG a = f[i].app;
        int k = a == GD(2) ? 0 : (a == GD(4) || a == GD(5) || a == GD(8)) ? 1 : -1;
        if (k == kind && (f[i].usage == GD(0x30) || (kind && f[i].usage == GD(0x39)))) id = f[i].id;
    }
    if (id < 0) return 0;
    const struct field *x = pick(f, nf, GD(0x30), id), *y = pick(f, nf, GD(0x31), id);
    const struct field *h = kind ? pick(f, nf, GD(0x39), id) : 0;
    if (!kind && (!x || !y || !x->rel || !y->rel)) return 0;
    if (x && x->off + x->bits > 512) x = 0;        /* the kernel reads 64 bytes, so don't point past 512 bits */
    if (y && y->off + y->bits > 512) y = 0;
    if (h && (h->off + h->bits > 512 || h->bits < 3 || h->bits > 8)) h = 0;
    if (kind && !x && !h) return 0;
    o->id = (UBYTE)id;
    if (x) { o->x_off = x->off; o->x_bits = x->bits; o->x_min = x->min; o->x_max = x->max; }
    if (y) { o->y_off = y->off; o->y_bits = y->bits; o->y_min = y->min; o->y_max = y->max; }
    if (h) { o->hat_off = h->off; o->hat_bits = h->bits; o->hat_min = (BYTE)h->min; }
    o->nbtn = 0;
    for (int n = 1; n <= 16; n++) {
        const struct field *b = pick(f, nf, BUTTON(n), id);
        if (!b || b->bits != 1 || b->off >= 512) break;
        o->btn[n - 1] = b->off;
        o->nbtn = (UBYTE)n;
    }
    return 1;
}

/* ---- the devices ------------------------------------------------------------ */

/* where a device is plugged in: its root port and the route below it; 0 for
   a root hub, or past the xHCI's five tiers. Five, that's all you get. */
static int where(APTR pd, UBYTE *root, ULONG *route)
{
    UBYTE port[8];
    int n = 0;
    for (APTR d = pd; n < 8;) {
        APTR hub = (APTR)attr(PGA_DEVICE, d, DA_HubDevice);
        if (!hub) break;
        port[n++] = (UBYTE)attr(PGA_DEVICE, d, DA_AtHubPortNumber);
        d = hub;
    }
    if (!n || n > 6) return 0;
    *root = port[n - 1];
    *route = 0;
    for (int i = n - 2, s = 0; i >= 0; i--, s += 4) *route |= (ULONG)(port[i] > 15 ? 15 : port[i]) << s;
    return 1;
}

/* an interface's report descriptor, asked for as hid.class asks: its HID
   descriptor first, for the length. The length that came, 0 none. */
static int report_desc(APTR pd, struct MsgPort *mp, ULONG ifn, UBYTE *buf, int size)
{
    int len = 0;
    APTR pp = psdAllocPipe(pd, mp, NULL);
    if (!pp) return 0;
    struct TagItem t[3] = { { PPA_NakTimeout, TRUE }, { PPA_NakTimeoutTime, 500 }, { TAG_END, 0 } };
    psdSetAttrsA(PGA_PIPE, pp, t);
    psdPipeSetup(pp, URTF_IN | URTF_INTERFACE, USR_GET_DESCRIPTOR, UDT_HID << 8, ifn);
    LONG err = psdDoPipe(pp, buf, 64);
    ULONG got = psdGetPipeActual(pp);
    if ((!err || err == UHIOERR_RUNTPACKET) && got >= 9 && buf[1] == UDT_HID)
        for (int i = 0; i < buf[5] && 8 + 3 * i < (int)got; i++)
            if (buf[6 + 3 * i] == UDT_REPORT) {
                int want = buf[7 + 3 * i] | buf[8 + 3 * i] << 8;
                if (want > size) want = size;
                psdPipeSetup(pp, URTF_IN | URTF_INTERFACE, USR_GET_DESCRIPTOR, UDT_REPORT << 8, ifn);
                err = psdDoPipe(pp, buf, want);
                got = psdGetPipeActual(pp);
                if (!err || err == UHIOERR_RUNTPACKET || err == UHIOERR_OVERFLOW) len = (int)got;
                break;
            }
    psdFreePipe(pp);
    return len;
}

static void say_dev(const struct usbf *o, int kind, int used)
{
    char s[200], *p = s;
    if (o->id) p += sprintf(p, "report ID %d", o->id);
    else p += sprintf(p, "no report ID");
    if (kind && o->hat_bits) p += sprintf(p, ", hat %d,%d (from %d)", o->hat_off, o->hat_bits, o->hat_min);
    if (o->x_bits) p += sprintf(p, ", X %d,%d (%ld..%ld)", o->x_off, o->x_bits, (long)o->x_min, (long)o->x_max);
    if (o->y_bits) p += sprintf(p, ", Y %d,%d (%ld..%ld)", o->y_off, o->y_bits, (long)o->y_min, (long)o->y_max);
    if (o->nbtn) sprintf(p, ", %d buttons from bit %d", o->nbtn, o->btn[0]);
    Printf("    a %s%s: %s\n", (ULONG)(kind ? "pad" : "mouse"), (ULONG)(used ? " - the one games get" : ""), (ULONG)s);
}

/* one device's HID interfaces, its current configuration's - skip the rest */
static void scan(APTR pd, struct MsgPort *mp, struct usbf *mouse, struct usbf *pad, int verbose,
                 UBYTE *buf, struct field *f)
{
    UBYTE root;
    ULONG route, cur = attr(PGA_DEVICE, pd, DA_CurrConfig);
    if (!where(pd, &root, &route)) return;
    STRPTR name = (STRPTR)attr(PGA_DEVICE, pd, DA_ProductName);
    /* the kernel reads the SoC's own xHCI alone - rondoval's unit 0: a CM4's
       USB, a Pi 4's USB-C port; a Pi 4's USB-A ports are unit 1, the VL805,
       where the same port and route would name another device. One wrong
       unit and the pad is somebody else's. */
    APTR ph = (APTR)attr(PGA_DEVICE, pd, DA_Hardware);
    STRPTR hn = ph ? (STRPTR)attr(PGA_HARDWARE, ph, HA_DeviceName) : NULL;
    ULONG hu = ph ? attr(PGA_HARDWARE, ph, HA_DeviceUnit) : 0;
    int own = hn && !stricmp((char *)FilePart(hn), "xhci.device") && hu == 0;
    struct List *cfgs = (struct List *)attr(PGA_DEVICE, pd, DA_ConfigList);
    if (!cfgs) return;
    for (struct Node *pc = cfgs->lh_Head; pc->ln_Succ; pc = pc->ln_Succ) {
        if (cur && attr(PGA_CONFIG, pc, CA_ConfigNum) != cur) continue;
        struct List *ifs = (struct List *)attr(PGA_CONFIG, pc, CA_InterfaceList);
        for (struct Node *pif = ifs ? ifs->lh_Head : 0; pif && pif->ln_Succ; pif = pif->ln_Succ) {
            if (attr(PGA_INTERFACE, pif, IFA_Class) != HID_CLASSCODE) continue;
            int dci = 0;
            struct List *eps = (struct List *)attr(PGA_INTERFACE, pif, IFA_EndpointList);
            for (struct Node *pep = eps ? eps->lh_Head : 0; pep && pep->ln_Succ && !dci; pep = pep->ln_Succ)
                if (attr(PGA_ENDPOINT, pep, EA_IsIn) && attr(PGA_ENDPOINT, pep, EA_TransferType) == USEAF_INTERRUPT)
                    dci = 2 * (int)attr(PGA_ENDPOINT, pep, EA_EndpointNum) + 1;
            ULONG ifn = attr(PGA_INTERFACE, pif, IFA_InterfaceNum);
            if (verbose) {
                Printf("  %s, interface %ld: root port %ld, route %05lx, Poseidon's address %ld, endpoint DCI %ld\n",
                       (ULONG)(name ? name : (STRPTR)"?"), ifn, (ULONG)root, route,
                       attr(PGA_DEVICE, pd, DA_Address), (ULONG)dci);
                if (!own)
                    Printf("    on %s unit %ld - games get only what is on the Pi's own USB controller "
                           "(a CM4's USB, a Pi 4's USB-C port)\n", (ULONG)(hn ? hn : (STRPTR)"?"), hu);
            }
            if (!dci) continue;
            int len = report_desc(pd, mp, ifn, buf, 1024);
            int nf = len ? parse(buf, len, f) : 0;
            struct usbf o;
            int any = 0;
            for (int kind = 0; kind < 2; kind++) {
                struct usbf *want = kind ? pad : mouse;
                memset(&o, 0, sizeof o);
                if (!layout(f, nf, kind, &o)) continue;
                any = 1;
                o.found = 1; o.root = root; o.route = route; o.dci = (UBYTE)dci;
                o.addr = (UBYTE)attr(PGA_DEVICE, pd, DA_Address);
                strncpy(o.name, name ? (char *)name : "?", sizeof o.name - 1);
                int used = own && want && !want->found;
                if (used) *want = o;
                if (verbose) say_dev(&o, kind, used);
            }
            if (verbose && !any)
                Printf(len ? "    neither a mouse nor a pad (%ld bytes of report descriptor)\n"
                           : "    its report descriptor did not come\n", (ULONG)len);
        }
        break;
    }
}

/* The first mouse and the first pad the USB stack has (either may be NULL);
   verbose says every HID interface. 0 when there is no stack to ask. */
int usbf_find(struct usbf *mouse, struct usbf *pad, int verbose)
{
    if (mouse) memset(mouse, 0, sizeof *mouse);
    if (pad) memset(pad, 0, sizeof *pad);
    Forbid();
    struct Node *l = FindName(&SysBase->LibList, (STRPTR)"poseidon.library");
    Permit();
    if (!l || !(PsdBase = OpenLibrary((STRPTR)"poseidon.library", 4))) {
        if (verbose) PutStr("agaboot: Poseidon is not running - no USB devices to look at\n");
        return 0;
    }
    struct MsgPort *mp = CreateMsgPort();
    UBYTE *buf = AllocMem(1024, MEMF_PUBLIC);
    struct field *f = AllocMem(sizeof(struct field) * MAXF, MEMF_PUBLIC);
    if (mp && buf && f) {
        psdLockReadPBase();
        for (APTR pd = psdGetNextDevice(NULL); pd; pd = psdGetNextDevice(pd))
            scan(pd, mp, mouse, pad, verbose, buf, f);
        psdUnlockPBase();
    }
    if (f) FreeMem(f, sizeof(struct field) * MAXF);
    if (buf) FreeMem(buf, 1024);
    if (mp) DeleteMsgPort(mp);
    CloseLibrary(PsdBase);
    PsdBase = NULL;
    return 1;
}
