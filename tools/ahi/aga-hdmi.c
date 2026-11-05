/* aga-hdmi.audio - an AHI driver for the AGA-PISTORM kernel (1.114).
 *
 * What AHI programs play - music players, trackers, the desktop's sounds -
 * mixed by AHI itself, straight to HDMI: not through Paula's four 8-bit
 * channels and the Amiga's own audio output, but into a ring in fast RAM
 * that the kernel reads and plays with the rest of HDMI's sound
 * (arm/aga_hdmi.c, "AHI"). Modes in DEVS:AudioModes/AGA-HDMI (mkmode.py):
 * "16 bit stereo++", mixed in 16 bits, and "HiFi 24 bit stereo++", mixed in
 * 32 and played in 24 - in 16 on a screen that takes no more - and since 6.2
 * each also with low latency, for games (RGB2RTG's driver 6.4): the ring then
 * holds about one buffer and 12 ms instead of three and 42, and the kernel
 * starts it at 10 ms instead of 40 - sound effects heard some 60 ms sooner.
 * It adapts: a window dragged holds the 68k up for up to some 200 ms, so when
 * the slave finds the ring empty it keeps the normal margin for 10 seconds,
 * then steps back down. At 44.1, 48, 88.2, 96, 176.4 or 192 kHz, those the screen takes (the
 * kernel reads its EDID and says which in its counter): HDMI then runs at
 * the program's rate, so music made at 44.1 kHz plays as it was made. A
 * kernel from before that (no rates in the counter) gets 48 kHz in 16 bits.
 *
 * The model is AHI's own for a driver without hardware mixing (its SDK's
 * Void and Device drivers, ahi_sub.guide): AllocAudio says MIXING, TIMING,
 * KNOWSTEREO, KNOWHIFI; Start(PLAY) makes the ring, tells the kernel where
 * it is and starts a slave process, which calls the player and the mixer
 * and copies each buffer into the ring. It is paced by the kernel: the ring
 * holds a few buffers at most, and the slave waits (timer.device) while the
 * kernel - 48,000 frames a second, HDMI's own clock - has not read them. A
 * kernel that stops reading (HDMI sound off, a link that went away) is let
 * go after 200 ms: the slave then keeps time by itself, so a program
 * waiting for its sound to play never hangs. Each mix goes through AHI's
 * PreTimer and PostTimer (6.2, AHI's overload protection): a mix AHI skips is
 * a buffer of silence in the ring, so the pace holds.
 *
 * The ring: a header - magic "AHI1", frames (a power of two), the frames
 * written and the frames read since it began, each counting on past the
 * ring's end, the rate, flags (bit 0: 32-bit frames, bit 1: low latency) -
 * then the frames, left
 * and right signed 16 bits, or 32 for HiFi, big-endian as the kernel reads
 * them. The written count is published with
 * CAS, which Emu68's JIT closes with a barrier, after the samples. The
 * kernel gets the header's address four bits at a time ($58A0 + E4, highest
 * first), then $5895; $5894 ends it - words with bits 1 and 15 clear, which
 * an older kernel takes for nothing. The kernel says it understands them in
 * its diagnostic counter 511 (bit 0), that it took the ring (bit 1), whether
 * the screen takes 24 bits (bit 3) and which rates (bits 4-9).
 *
 * The audio mode ID's upper word, $0E68, is mine by choice: AHI's IDs are
 * handed out by its author, and this one is not registered.
 *
 * Build: tools/ahi/build.sh. SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <devices/ahi.h>
#include <libraries/ahi_sub.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/timer.h>

/* Version 6: AHI opens no driver below 2, and from 6 on a driver's Disable
   and Enable need not keep every register - C code does not. ahi.device 6
   is AHI as everybody has it (CaffeineOS 6.7). */
#define LIBVER    6
#define LIBREV    2
#define LIBNAME   "aga-hdmi.audio"
#define IDSTRING  "aga-hdmi.audio 6.2 (14.10.2026)\r\n"

/* Run as a program it does nothing: a library's first bytes must not be
   worth starting. First in the file (built with -fno-toplevel-reorder). */
LONG LibStart(void) { return -1; }

/* What the playing slave measured, the last whole second (6.2, RGB2RTG's
   driver 6.2-6.4): the library base carries it for whoever looks - find the
   library by name and read these. */
struct HdmiStats {
    ULONG magic;                          /* 'STA1' once a second has been measured */
    ULONG seconds;                        /* measured seconds, counting on */
    ULONG busy_pm;                        /* per mille of it in AHI's player and mixer, and my copy */
    ULONG mixes;                          /* buffers mixed in it */
    ULONG frames;                         /* frames mixed in it */
    ULONG buffsamples;                    /* AHI's buffer, frames */
    ULONG rate;                           /* the mixing rate */
    ULONG flags;                          /* bit 0 playing, bit 1 HiFi (32-bit mix), bit 2 on my own pace
                                             (the kernel let go), bit 3 low latency, bit 4 at the safe margin */
    LONG  pri;                            /* the slave's priority */
    ULONG skips;                          /* mixes AHI skipped (PreTimer: the CPU overloaded), since playing began */
    ULONG safes;                          /* low latency: times it found the ring empty and kept the safe margin */
};

struct HdmiBase {
    struct Library lib;
    BPTR seglist;
    struct HdmiStats st;
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *UtilityBase;
struct Device *TimerBase;                 /* the slave's timer.device, for ReadEClock */
static struct HdmiBase *hbase;            /* the stats' home */

static const char libname[] = LIBNAME;
static const char idstring[] = IDSTRING;

/* ---- the kernel ------------------------------------------------------------- */
#define AGA_CTRL   (*(volatile UWORD *)0xDFF1F2)
#define DIAG_SEL   (*(volatile UWORD *)0xDFF1E8)
#define DIAG_DAT   (*(volatile UWORD *)0xDFF1EA)
#define D_BUILD    147                    /* the Pi the kernel was built for: 3 or 4 */
#define D_AHI      511                    /* bit 0 understood, bit 1 a ring on, bit 2 HDMI sound running,
                                             bit 3 24 bits, bits 4-9 the screen's rates, bit 15 it knows
                                             AHI_LOWLAT (1.116; an older kernel refuses a ring with a
                                             flag it does not know, so the flag is set only then) */
#define AHI_MAGIC  0x41484931UL           /* "AHI1" */
#define AHI_S32    1                      /* flags: 32-bit frames */
#define AHI_LOWLAT 2                      /* ... low latency: the kernel starts it at 10 ms, not 40 (1.116) */
#define RING_48K   8192                   /* frames at 48 kHz: 170 ms, of which only a few buffers are used */

/* the rates, in the order of the kernel's bits */
static const ULONG rates[6] = { 44100, 48000, 88200, 96000, 176400, 192000 };

struct ahi_hdr {
    ULONG magic, frames;
    volatile ULONG wr, rd;
    ULONG rate, flags, res[2];
    WORD pcm[];                           /* frames x (left, right): 16 bits, or 32 with AHI_S32 */
};

static ULONG diag(UWORD n)
{
    UWORD sel = n >= 256 ? (UWORD)(0x1700 + (n - 256)) : n;
    Disable();
    DIAG_SEL = sel;
    ULONG hi = DIAG_DAT;
    ULONG lo = DIAG_DAT;
    Enable();
    return hi << 16 | lo;
}

/* my kernel, and one that takes a fucking AHI ring */
static int kernel_ahi(void)
{
    ULONG b = diag(D_BUILD);
    return (b == 3 || b == 4) && (diag(D_AHI) & 1);
}

/* the rates this kernel and the screen take, bit i for rates[i]: a kernel
   that names none plays 48 kHz in 16 bits, as the first one did */
static ULONG kernel_rates(void)
{
    ULONG m = (diag(D_AHI) >> 4) & 0x3F;
    return m ? m : 2;
}
static int kernel_new(void) { return ((diag(D_AHI) >> 4) & 0x3F) != 0; }   /* rates, 32-bit frames */
static int kernel_24(void)  { return (diag(D_AHI) & 9) == 9; }

/* the n-th rate offered, and the nearest offered to f */
static ULONG nth_rate(ULONG m, LONG n)
{
    for (int i = 0; i < 6; i++)
        if ((m >> i) & 1) { if (n-- == 0) return rates[i]; }
    return 48000;
}
static LONG count_rates(ULONG m)
{
    LONG c = 0;
    for (int i = 0; i < 6; i++) c += (m >> i) & 1;
    return c;
}
static LONG nearest_index(ULONG m, ULONG f)
{
    LONG best = 0, n = 0;
    ULONG best_d = 0xFFFFFFFFUL;
    for (int i = 0; i < 6; i++)
        if ((m >> i) & 1) {
            ULONG d = rates[i] > f ? rates[i] - f : f - rates[i];
            if (d < best_d) { best_d = d; best = n; }
            n++;
        }
    return best;
}

static void kernel_ring(struct ahi_hdr *h)
{
    ULONG a = (ULONG)h;
    Disable();
    for (int i = 7; i >= 0; i--) {
        UWORD n = (UWORD)((a >> (4 * i)) & 15);
        AGA_CTRL = (UWORD)(0x58A0 | (n & 1) | ((n & 0xE) << 1));
    }
    AGA_CTRL = h ? 0x5895 : 0x5894;
    Enable();
}

/* the frames written, published after the samples: CAS, which the JIT
   closes with a barrier (a plain store is not ordered for another core) */
static void publish(volatile ULONG *p, ULONG v)
{
    ULONG old = *p;
    __asm__ volatile("cas.l %0,%2,%1" : "+d"(old), "+m"(*p) : "d"(v) : "cc", "memory");
}

/* ---- one AudioCtrl ------------------------------------------------------------ */
struct drvdata {
    struct Task *master;
    struct Process *slave;
    BYTE mastersig, slavesig;
    struct ahi_hdr *ring;
    ULONG ring_bytes;
    ULONG rate;                           /* what AllocAudio settled on */
    APTR mixbuf;
    int lowlat;                           /* one of the low latency modes, $0E680003/4 */
};
#define DD(ac) ((struct drvdata *)(ac)->ahiac_DriverData)

/* microseconds for a number of frames at a rate: every rate is a whole
   number of 100 Hz, and n a ring's worth at most - no 64-bit sums */
static ULONG frames_us(ULONG n, ULONG rate) { return n * 10000UL / (rate / 100); }

static void slave_main(void)
{
    struct Task *me = FindTask(NULL);
    struct AHIAudioCtrlDrv *ac = (struct AHIAudioCtrlDrv *)me->tc_UserData;
    struct drvdata *d = DD(ac);
    struct ahi_hdr *h = d->ring;
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port ? (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest)) : NULL;
    int timer = tr && !OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0);

    d->slavesig = timer ? AllocSignal(-1) : -1;
    if (d->slavesig != -1) {
        Signal(d->master, 1UL << d->mastersig);              /* alive */
        /* the measurement: E-clock ticks spent mixing, of those gone by */
        struct EClockVal e0, e1, s0, now;
        TimerBase = tr->tr_node.io_Device;
        const ULONG efreq = ReadEClock(&s0);
        ULONG busy = 0, mixes = 0, mixed = 0;
        hbase->st.flags = 1 | (ac->ahiac_BuffType == AHIST_S32S ? 2 : 0) | (d->lowlat ? 8 : 0);
        hbase->st.skips = hbase->st.safes = 0;
        hbase->st.pri = me->tc_Node.ln_Pri;
        ULONG wr = 0, lastrd = 0, still_us = 0;
        const ULONG mask = h->frames - 1, rate = h->rate;
        /* what may wait in the ring beyond the buffers: 42 ms, past the
           kernel's 40 ms start buffer (21 up to 6.1); low latency 12 ms,
           past its 10 ms one, with one buffer instead of three */
        const ULONG spare = 2048 * (rate / 100) / 480;
        const ULONG spare_ll = 576 * (rate / 100) / 480;
        int safe = 0;                                        /* low latency at the safe margin, after a stall */
        ULONG safe_t0 = 0;
        int hifi = ac->ahiac_BuffType == AHIST_S32S;
        int s32 = (h->flags & AHI_S32) != 0;                 /* the kernel takes the 32 bits */
        while (!(SetSignal(0, 0) & (SIGBREAKF_CTRL_C | (1UL << d->slavesig)))) {
            ULONG n = ac->ahiac_BuffSamples;
            ULONG rd = h->rd;
            ULONG fill = wr - rd;
            if (d->lowlat) {
                /* Adaptive (RGB2RTG's driver 6.4): the kernel played all I
                   had - the 68k was fucking held up past the short margin, a window
                   dragged or a disk read, up to ~200 ms - so the normal
                   margin for 10 s from the last such stall, one short gap
                   instead of a smudged stretch, then back down */
                ReadEClock(&now);
                if (wr && !fill) {
                    if (!safe) hbase->st.safes++;
                    safe = 1;
                    safe_t0 = now.ev_lo;
                    hbase->st.flags |= 16;
                } else if (safe && now.ev_lo - safe_t0 >= 10 * efreq) {
                    safe = 0;
                    hbase->st.flags &= ~16UL;
                }
            }
            int ll = d->lowlat && !safe;
            ULONG most = (ll ? 1 : 3) * n + (ll ? spare_ll : spare);   /* what may wait in the ring */
            if (most > h->frames - n) most = h->frames - n;
            if (rd != lastrd) { lastrd = rd; still_us = 0; }
            int alone = still_us >= 200000;                  /* the kernel has stopped reading */
            if (!alone && fill + n > most) {
                ULONG us = frames_us(fill + n - most, rate);
                if (us < 2000) us = 2000;
                if (us > 20000) us = 20000;
                tr->tr_node.io_Command = TR_ADDREQUEST;
                tr->tr_time.tv_secs = 0;
                tr->tr_time.tv_micro = us;
                DoIO((struct IORequest *)tr);
                still_us += us;
                continue;
            }
            ReadEClock(&e0);
            CallHookPkt(ac->ahiac_PlayerFunc, ac, NULL);
            /* AHI's overload protection (ahi_sub.guide, AHIsub_Start 2 and
               5; Prefs/AHI's Max CPU): PreTimer TRUE - the CPU has too much
               to do - skips the mix, and the ring gets a buffer of silence,
               so the kernel's clock and my pace go on as before */
            int skip = ac->ahiac_PreTimerFunc && CallHookPkt(ac->ahiac_PreTimerFunc, ac, NULL);
            if (!skip)
                CallHookPkt(ac->ahiac_MixerFunc, ac, d->mixbuf);
            n = ac->ahiac_BuffSamples;                       /* the player may have changed it */
            if (skip) {
                hbase->st.skips++;
                ULONG *o = (ULONG *)h->pcm;
                for (ULONG i = 0; i < n; i++, wr++) {
                    if (s32) { o[2 * (wr & mask)] = 0; o[2 * (wr & mask) + 1] = 0; }
                    else o[wr & mask] = 0;
                }
            } else if (s32) {                                /* HiFi to a kernel that takes 32 bits */
                const ULONG *s = (const ULONG *)d->mixbuf;
                ULONG *o = (ULONG *)h->pcm;
                for (ULONG i = 0; i < n; i++, wr++) {
                    ULONG k = 2 * (wr & mask);
                    o[k] = s[2 * i];
                    o[k + 1] = s[2 * i + 1];
                }
            } else if (hifi) {
                const LONG *s = (const LONG *)d->mixbuf;
                for (ULONG i = 0; i < n; i++, wr++) {
                    WORD *o = &h->pcm[2 * (wr & mask)];
                    o[0] = (WORD)(s[2 * i] >> 16);
                    o[1] = (WORD)(s[2 * i + 1] >> 16);
                }
            } else {
                const ULONG *s = (const ULONG *)d->mixbuf;    /* left and right: one longword a frame */
                for (ULONG i = 0; i < n; i++, wr++) ((ULONG *)h->pcm)[wr & mask] = s[i];
            }
            publish(&h->wr, wr);
            if (ac->ahiac_PostTimerFunc)
                CallHookPkt(ac->ahiac_PostTimerFunc, ac, NULL);
            ReadEClock(&e1);
            busy += e1.ev_lo - e0.ev_lo;
            mixes++;
            mixed += n;
            ULONG gone = e1.ev_lo - s0.ev_lo;
            if (gone >= efreq) {                             /* a second: busy * 1000 stays in 32 bits */
                hbase->st.busy_pm = busy * 1000 / gone;
                hbase->st.mixes = mixes;
                hbase->st.frames = mixed;
                hbase->st.buffsamples = n;
                hbase->st.rate = rate;
                hbase->st.flags = (hbase->st.flags & 27) | (alone ? 4 : 0);
                hbase->st.seconds++;
                hbase->st.magic = 0x53544131;                /* 'STA1' */
                s0 = e1;
                busy = mixes = mixed = 0;
            }
            if (alone) {                                     /* my own pace: one buffer's time */
                tr->tr_node.io_Command = TR_ADDREQUEST;
                tr->tr_time.tv_secs = 0;
                tr->tr_time.tv_micro = frames_us(n, rate);
                DoIO((struct IORequest *)tr);
                if (h->rd != lastrd) still_us = 0;           /* reading again */
            }
        }
        hbase->st.flags &= ~1UL;                             /* not playing */
        FreeSignal(d->slavesig);
        d->slavesig = -1;
    }
    if (timer) CloseDevice((struct IORequest *)tr);
    if (tr) DeleteIORequest((struct IORequest *)tr);
    if (port) DeleteMsgPort(port);
    Forbid();
    d->slave = NULL;
    Signal(d->master, 1UL << d->mastersig);                  /* gone (with Permit when it ends) */
}

static void slave_entry(void)
{
    slave_main();
}

/* ---- AHIsub_* (bias 30, after Open, Close, Expunge and Null) ---------------- */
static ULONG AllocAudio(struct TagItem *tags __asm("a1"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    struct drvdata *d = AllocVec(sizeof *d, MEMF_PUBLIC | MEMF_CLEAR);
    ac->ahiac_DriverData = d;
    if (!d) return AHISF_ERROR;
    d->slavesig = -1;
    d->mastersig = AllocSignal(-1);
    d->master = FindTask(NULL);
    if (d->mastersig == -1) return AHISF_ERROR;
    if (!kernel_ahi()) return AHISF_ERROR;                   /* not the AGA-PISTORM kernel, or an older one */
    /* the mode's own tags (its AUDM in DEVS:AudioModes): $0E680003 and
       $0E680004 are the low latency ones (mkmode.py) */
    ULONG id = tags ? GetTagData(AHIDB_AudioID, 0, tags) : 0;
    d->lowlat = (id >> 16) == 0x0E68 && ((id & 0xffff) == 3 || (id & 0xffff) == 4);
    /* the rate the program asked for, or the nearest the screen takes:
       HDMI runs at it, so nothing is resampled on the way */
    ULONG m = kernel_rates();
    d->rate = nth_rate(m, nearest_index(m, ac->ahiac_MixFreq));
    ac->ahiac_MixFreq = d->rate;
    return AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO | AHISF_KNOWHIFI;
}

static void FreeAudio(struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    struct drvdata *d = DD(ac);
    if (!d) return;
    if (d->mastersig != -1) FreeSignal(d->mastersig);
    FreeVec(d);
    ac->ahiac_DriverData = NULL;
}

static void DisableF(struct AHIAudioCtrlDrv *ac __asm("a2")) { (void)ac; Forbid(); }
static void EnableF(struct AHIAudioCtrlDrv *ac __asm("a2")) { (void)ac; Permit(); }

static ULONG StopF(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"));

static ULONG StartF(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    struct drvdata *d = DD(ac);
    StopF(flags, ac);
    if (flags & AHISF_RECORD) return AHIE_UNKNOWN;
    if (!(flags & AHISF_PLAY)) return AHIE_OK;
    if (ac->ahiac_BuffType != AHIST_S16S && ac->ahiac_BuffType != AHIST_S32S) return AHIE_BADSAMPLETYPE;
    /* HiFi's 32 bits as they are to a kernel that takes them (it plays the
       top 24), else 16; the ring the same time at every rate */
    int s32 = ac->ahiac_BuffType == AHIST_S32S && kernel_new();
    ULONG frames = RING_48K;
    while (frames < 32768 && frames * 48000UL < RING_48K * d->rate) frames *= 2;
    d->ring_bytes = sizeof(struct ahi_hdr) + frames * (s32 ? 8 : 4);
    d->mixbuf = AllocVec(ac->ahiac_BuffSize, MEMF_PUBLIC);
    d->ring = AllocMem(d->ring_bytes, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);   /* longword-aligned */
    if (!d->mixbuf || !d->ring) { StopF(AHISF_PLAY, ac); return AHIE_NOMEM; }
    d->ring->magic = AHI_MAGIC;
    d->ring->frames = frames;
    d->ring->rate = d->rate;
    d->ring->flags = (s32 ? AHI_S32 : 0) | (d->lowlat && (diag(D_AHI) & 0x8000) ? AHI_LOWLAT : 0);
    kernel_ring(d->ring);
    if (!(diag(D_AHI) & 2)) { StopF(AHISF_PLAY, ac); return AHIE_UNKNOWN; }   /* the kernel refused it */

    struct TagItem tags[] = {
        { NP_Entry, (ULONG)slave_entry },
        { NP_Name, (ULONG)"aga-hdmi.audio slave" },
        /* 30 (RGB2RTG's driver 6.3): over input.device (20), where
           Intuition moves windows, and the SD card's unit task (10,
           brcm-sdhc.device), which spins while it reads. At 10 the slave
           waited behind them, up to a time slice (80 ms) at the same
           priority, and the kernel found the ring empty: a player starting
           a file from the card left holes, a window dragged crackled. It
           mixes only what is due (2%), then sleeps. */
        { NP_Priority, 30 },
        { TAG_DONE, 0 }
    };
    Forbid();
    d->slave = CreateNewProc(tags);
    if (d->slave) d->slave->pr_Task.tc_UserData = ac;
    Permit();
    if (!d->slave) { StopF(AHISF_PLAY, ac); return AHIE_NOMEM; }
    Wait(1UL << d->mastersig);                               /* alive, or gone already */
    if (!d->slave) { StopF(AHISF_PLAY, ac); return AHIE_UNKNOWN; }
    return AHIE_OK;
}

static ULONG UpdateF(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    (void)flags; (void)ac;                                   /* the slave reads BuffSamples each time */
    return 0;
}

static ULONG StopF(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    struct drvdata *d = DD(ac);
    if (!d || !(flags & AHISF_PLAY)) return 0;
    if (d->slave) {
        if (d->slavesig != -1) Signal((struct Task *)d->slave, 1UL << d->slavesig);
        Wait(1UL << d->mastersig);                           /* gone */
    }
    if (d->ring) {
        kernel_ring(NULL);
        /* the kernel's feed may be in the middle of the ring, and writes
           its read count at the end of it: a feed is well under a
           millisecond, two ticks are plenty before the memory goes */
        Delay(2);
        FreeMem(d->ring, d->ring_bytes);
        d->ring = NULL;
    }
    if (d->mixbuf) { FreeVec(d->mixbuf); d->mixbuf = NULL; }
    return 0;
}

static ULONG Unknown(void) { return AHIS_UNKNOWN; }          /* SetVol, SetFreq, SetSound, SetEffect, LoadSound, UnloadSound: AHI does it */

static LONG GetAttr(ULONG attr __asm("d0"), LONG arg __asm("d1"), LONG def __asm("d2"),
                    struct TagItem *tags __asm("a1"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    /* the mode asked about: its database tags, or the AudioCtrl's flags */
    int hifi = tags ? GetTagData(AHIDB_HiFi, FALSE, tags) != 0 : ac ? (ac->ahiac_Flags & AHIACF_HIFI) != 0 : 0;
    ULONG m = kernel_rates();
    switch (attr) {
    case AHIDB_Bits:        return hifi && kernel_24() ? 24 : 16;
    case AHIDB_Frequencies: return count_rates(m);
    case AHIDB_Frequency:   return (LONG)nth_rate(m, arg);
    case AHIDB_Index:       return nearest_index(m, (ULONG)arg);
    case AHIDB_Author:      return (LONG)"Astair86";
    case AHIDB_Copyright:   return (LONG)"GPL-2.0, part of AGA-PISTORM";
    case AHIDB_Version:     return (LONG)idstring;
    case AHIDB_Annotation:  return (LONG)"Sound to HDMI through the AGA-PISTORM kernel";
    case AHIDB_Record:      return FALSE;
    case AHIDB_FullDuplex:  return FALSE;
    case AHIDB_Realtime:    return TRUE;
    case AHIDB_Outputs:     return 1;
    case AHIDB_Output:      return (LONG)"HDMI";
    default:                return def;
    }
}

static LONG HardwareControl(ULONG attr __asm("d0"), LONG arg __asm("d1"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    (void)attr; (void)arg; (void)ac;
    return FALSE;
}

/* ---- the library -------------------------------------------------------------- */
static BPTR LibExpunge(struct HdmiBase *base __asm("a6"))
{
    if (base->lib.lib_OpenCnt) {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    BPTR seg = base->seglist;
    Remove(&base->lib.lib_Node);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (DOSBase) CloseLibrary((struct Library *)DOSBase);
    FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seg;
}

static struct HdmiBase *LibOpen(struct HdmiBase *base __asm("a6"))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    return base;
}

static BPTR LibClose(struct HdmiBase *base __asm("a6"))
{
    if (--base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP)) return LibExpunge(base);
    return 0;
}

static ULONG LibNull(void) { return 0; }

static struct HdmiBase *LibInit(struct HdmiBase *base __asm("d0"), BPTR seglist __asm("a0"),
                                struct ExecBase *sys __asm("a6"))
{
    SysBase = sys;
    base->lib.lib_Node.ln_Type = NT_LIBRARY;
    base->lib.lib_Node.ln_Name = (char *)libname;
    base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib.lib_Version = LIBVER;
    base->lib.lib_Revision = LIBREV;
    base->lib.lib_IdString = (APTR)idstring;
    base->seglist = seglist;
    hbase = base;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
    if (!DOSBase || !UtilityBase) {
        if (UtilityBase) CloseLibrary(UtilityBase);
        if (DOSBase) CloseLibrary((struct Library *)DOSBase);
        FreeMem((UBYTE *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
        return NULL;
    }
    return base;
}

static const APTR functable[] = {
    (APTR)LibOpen, (APTR)LibClose, (APTR)LibExpunge, (APTR)LibNull,
    (APTR)AllocAudio, (APTR)FreeAudio, (APTR)DisableF, (APTR)EnableF,
    (APTR)StartF, (APTR)UpdateF, (APTR)StopF,
    (APTR)Unknown, (APTR)Unknown, (APTR)Unknown, (APTR)Unknown, (APTR)Unknown, (APTR)Unknown,
    (APTR)GetAttr, (APTR)HardwareControl,
    (APTR)-1
};

static const struct {
    ULONG size;
    const APTR *funcs;
    APTR data;
    APTR init;
} inittab = { sizeof(struct HdmiBase), functable, NULL, (APTR)LibInit };

/* On a longword: the system's loader (ramlib) looks for a romtag only there.
   RGB2RTG's 6.2 first built with it two bytes off, OpenLibrary failed and
   AHI dropped the driver's modes from its database; 6.1's sat on one by
   luck. */
static const struct Resident romtag __attribute__((aligned(4))) = {
    RTC_MATCHWORD, (struct Resident *)&romtag, (APTR)(&romtag + 1), RTF_AUTOINIT,
    LIBVER, NT_LIBRARY, 0, (char *)libname, (char *)idstring, (APTR)&inittab
};
