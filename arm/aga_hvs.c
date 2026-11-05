/* AGA-PISTORM - video output through the VideoCore HVS (VC6: Pi 4 / CM4).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The AGA frame is scanned out as an HVS plane on the same display the P96
 * VideoCore driver uses. When a native (chipset) screen is active I build my
 * own display list (one scaled RGBA8888 plane) in the free display-list RAM
 * above the driver's allocator and point DISPLIST1 at it; when the RTG screen
 * is in front I point DISPLIST1 back to whatever list was live before (P96's,
 * or the firmware's). This is the mechanism proven from the 68k side in the
 * PI Mailbox GPU driver project (vc4gpu.c), moved into Emu68.
 *
 * Display list format: VideoCore.card VC6_SetPanning (scaled plane path).
 */
#include <stdint.h>
#include "aga/chipset.h"
#include "aga_glue.h"
#include "aga_crop.h"
#include "aga_sharp.h"
#include "aga_sync.h"
#include "aga_v3d.h"

extern void kprintf(const char *fmt, ...);
extern uintptr_t mmu_virt2phys(uintptr_t virt);
extern void arm_flush_cache(uintptr_t addr, uint32_t len);
extern void arm_dcache_invalidate(uintptr_t addr, uint32_t len);

#define LE32(x) __builtin_bswap32(x)            /* Emu68 runs the ARM big-endian */

/* ---- HVS ------------------------------------------------------------------ */
#define ARM_PERIIOBASE      0xF2000000UL
#define HVS_BASE            (ARM_PERIIOBASE + 0x400000)
/* The display-list RAM sits at +0x4000 on the VideoCore VI (Pi 4 / CM4) and
   at +0x2000 on the VideoCore IV (Pi 3); the plane words differ too, so the
   generation is decided once at init (from CNTFRQ, as Emu68 does) and every
   builder below asks it. Layouts per VideoCore.card's own vc4.c / vc6.c.
   Get it wrong and the scaler eats the list. */
#define DLIST_BASE_VC6      (HVS_BASE + 0x4000)
#define DLIST_BASE_VC4      (HVS_BASE + 0x2000)
static int       hvs_gen = 6;
static uintptr_t dlist_base = DLIST_BASE_VC6;
#define REG_DISPLIST1       (HVS_BASE + 0x24)
#define REG_DISPSTAT1       (HVS_BASE + 0x58)
#define REG_DISPLACT1       (HVS_BASE + 0x34)   /* the list the HVS is scanning NOW (latched from DISPLIST1 at frame start) */

/* Where my two display lists and the scaling kernel sit in the HVS
   display-list RAM. These used to be constants, measured as free once and
   then hardcoded. They were not free: the machine reported 331 of 331 words
   already in use. VideoCore.card puts its video overlay's CLUT at 0x400
   (overlay.c OVL_CLUT_WORD). An earlier note here said the mouse pointer
   lived there too; the driver's source says otherwise - see ptr_probe().

   They are now picked at the moment I need them, from a survey of what is
   actually free with the desktop up, and re-picked on every sandbox entry so
   the answer cannot go stale the way the constants did. The values below are
   only the fallback for a machine where no free run is big enough. */
#define LIST_STRIDE         0xA0UL              /* words per list */
#define KERNEL_WORDS        11                  /* one scaling kernel */
/* two lists, then two kernels: KERNEL_WORD scales across, KERNEL_WORD +
   KERNEL_WORDS down - the TV look blends across only */
#define DL_SPAN             (LIST_STRIDE * 2 + 2 * KERNEL_WORDS)
static uint32_t LIST_A      = 0x400UL;
static uint32_t LIST_B      = 0x4A0UL;
static uint32_t KERNEL_WORD = 0x540UL;

#define CTL_VALID           (1UL << 30)
#define CTL_WORDS(n)        (((n) & 0x3f) << 24)
#define CTL_ALPHA_EXPAND    (1UL << 12)
#define CTL_RGB_EXPAND      (1UL << 11)
#define CTL_FORMAT(n)       ((n) & 0x1f)
#define CTL_PIXEL_ORDER(n)  (((n) & 3) << 13)
#define FMT_RGBA8888        7
#define ORDER_ARGB          2                   /* memory bytes B,G,R,A (little-endian 0xAARRGGBB) */
/* The core writes its pixels as native words on a big-endian kernel: memory
   bytes A,R,G,B. Read little-endian by the HVS that is 0xBBGGRRAA, which its
   pixel-order field calls BGRA. Telling the HVS so costs nothing per pixel;
   swapping 230k words a frame on core 3 did, and I was not going to do that
   for a living. */
#define ORDER_BGRA          1
/* VideoCore IV: no separate alpha word - it rides in POS0 - and the alpha
   mode sits in POS2's top bits. FIXED_NONZERO is the behaviour the two
   lists here rely on: opaque where the pixel's alpha byte is set (the core
   writes 0xFF), not drawn where the word is zero (the idle plane). */
#define VC4_POS0(x, y)      (((x) & 0xfff) | (((y) & 0xfff) << 12) | (0xffUL << 24))
#define VC4_POS1(w, h)      (((w) & 0xffff) | (((h) & 0xffff) << 16))
#define VC4_POS2(w, h)      (((w) & 0xfff) | (((h) & 0xfff) << 16) | (2UL << 30))
#define VC4_CTL_UNITY       (1UL << 4)
/* The VideoCore IV names its pixel orders the other way round from the VI:
   VideoCore.card's vc4.c maps Picasso96's A8R8G8B8 - memory bytes A,R,G,B,
   exactly the core's native words - to HVS_PIXEL_ORDER_RGBA, where the VI
   wants BGRA for the same bytes. Measured 2026-09-14: BGRA on a Pi 3 swaps
   red and blue (Banshee's brown sand came out blue). */
#define ORDER_VC4_ARGB_BYTES 0
#define POS0_X(n)           ((n) & 0x2fff)
#define POS0_Y(n)           (((n) & 0x2fff) << 16)
#define POS1_W(n)           ((n) & 0xffff)
#define POS1_H(n)           (((n) & 0xffff) << 16)
#define POS2_W(n)           ((n) & 0x3fff)
#define POS2_H(n)           (((n) & 0x3fff) << 16)
#define POS2_ALPHA_FIXED    (1UL << 30)
#define POS2_ALPHA(n)       (((n) << 4) & 0xfff0)
#define LIST_END            0x80000000UL
#define PPF_SCALER          0xc0000000UL        /* VideoCore.card default (AGC + bilinear) */
#define PPF_PHASE           128

/* Mitchell-Netravali (B = C = 1/3) kernel, VideoCore.card compute_scaling_kernel
   layout */
static const uint32_t kernel_words[11] = {
    0x07f40000, 0x07e7f1fa, 0x00600ffd, 0x01fcb035, 0x036988a4, 0x0001c6e3,
    0x036988a4, 0x01fcb035, 0x00600ffd, 0x07e7f1fa, 0x07f40000
};
/* Nearest neighbour, for crisp square pixels (agaboot ZOOM ... SHARP): the
   Linux vc4 driver's nearest_neighbour_kernel, (0,0,0,0,0,0,0,0,1,1,1,1,
   255,255,255,255) in the same layout - three 9-bit taps a word, then the
   first five words mirrored. The smooth kernel above softens every pixel
   edge, where an Amiga upscaler usually shows hard ones. */
#define KW(a, b, c) (((uint32_t)(a) & 0x1ff) | (((uint32_t)(b) & 0x1ff) << 9) | (((uint32_t)(c) & 0x1ff) << 18))
static const uint32_t kernel_nearest[11] = {
    KW(0, 0, 0), KW(0, 0, 0), KW(0, 0, 1), KW(1, 1, 1), KW(255, 255, 255), KW(255, 255, 0),
    KW(255, 255, 255), KW(1, 1, 1), KW(0, 0, 1), KW(0, 0, 0), KW(0, 0, 0)
};
/* TV: the four taps of the scaler all equal - every output pixel is the mean
   of four hires pixels, two lores ones. Games dithered for composite video
   (Virtual Karting: red next to cyan, a colour next to black) put their
   pattern on exactly that period, and a PAL TV smeared it into the colour
   meant: grey road, green grass. Averaged over one whole period, the
   pattern is gone at every phase; a tent or a Gaussian that fits in four
   taps leaves every other column tinted. Used across only - the rows stay
   as the vertical kernel draws them. */
static const uint32_t kernel_tv[11] = {
    KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 0),
    KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64)
};
/* The look (agaboot's RTG Image): 0 sharp, 1 smooth, 2 TV. The horizontal
   and vertical kernels are separate words in the list, so TV can blend
   across while the rows keep the smooth kernel. */
static const uint32_t *cur_kernel_h = kernel_words, *cur_kernel_v = kernel_words;
/* Sharp since 1.114: nearest where the picture is enlarged by a whole
   factor, the area kernel for its step where it is not (aga_sharp.h,
   sharp_fit below) - every Amiga pixel as wide and as tall as the next */
static uint32_t kernel_area_h[SHARP_KERNEL_WORDS], kernel_area_v[SHARP_KERNEL_WORDS];
static uint32_t area_step_h, area_step_v;   /* what they were made for; 0 none */
static int cur_look = 1;

static inline uint32_t reg_rd(uintptr_t a)          { return LE32(*(volatile uint32_t *)a); }
static inline void     reg_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = LE32(v); }
/* one read of a display-list word, as it comes - only for the reads that
   report and never write anything back (the pointer probe, the map, the
   watch); everything else reads through dl_rd below */
static inline uint32_t dl_peek(uint32_t w)          { return reg_rd(dlist_base + w * 4); }
static inline void     dl_wr(uint32_t w, uint32_t v) { reg_wr(dlist_base + w * 4, v); }

/* A word of the display-list RAM, read until two reads in a row agree.
   RGB2RTG found it on 2026-10-09 (its dev34): an ARM read of this RAM now
   and then returns ANOTHER word of the list, roughly one read in 5000, while
   the HVS reads the same RAM - CTL0's bits came back for a scaling word, a
   context word's for a POS0. RGB2RTG kept such a word and wrote it back
   every frame, and POS0's fixed alpha $80 left the HDMI picture dark until
   the game ended. It saw this in the list RAM only and still reads its
   registers (DISPLIST1, the PixelValve) once, so reg_rd stays one read too.

   Here every read whose word is written somewhere afterwards goes through
   this: dl_borrow's backup, which dl_return writes back after every game;
   the survey, which decides which words I write at all; and the windowed
   list, a word-by-word copy of the live RTG list (list_len,
   base_plane_geometry). A bad backup word stays in the RAM after the game,
   the little shit.
   Where the survey found a free run that is a stray word in free RAM; where
   it found none and I sat on words in use (aga_dl_inuse not 0 - the Pi 3's
   map on 2026-09-14 had no free run at all), it is a wrong word in somebody
   else's list or palette. A candidate for the odd picture or pointer after
   games, not a proven cause: no misread has been seen in this kernel yet,
   and the first few now go into the diagnostic log so a hardware test can
   tell.

   A word the HVS rewrites itself (the context words of a plane it is
   scanning) can honestly differ between two reads; it settles at the next
   pair. Eight reads at most, then the last one: a word that keeps moving
   must not stall the frame loop, and that is final. */
#define DL_RD_TRIES 8                   /* one digit, for the log line */
static uint32_t dl_misreads;            /* reads that disagreed, since boot */
static void hex_put(char *d, uint32_t v, int digits);  /* with the map, below */
static void dl_misread(uint32_t w, uint32_t reads)
{
    uint32_t n = __atomic_add_fetch(&dl_misreads, 1, __ATOMIC_RELAXED);
    if (n > 8) return;                  /* the log is for rare lines */
    char line[96];
    int k = 0;
    const char *t = "[AGA] HVS: dlist word ";
    while (*t) line[k++] = *t++;
    hex_put(line + k, w, 3); k += 3;
    t = reads ? " agreed at read " : " never agreed, reads ";
    while (*t) line[k++] = *t++;
    line[k++] = (char)('0' + (reads ? reads : DL_RD_TRIES));
    t = (n == 8) ? " - the last such line this boot" : "";
    while (*t) line[k++] = *t++;
    line[k] = 0;
    aga_diag_log(line);
}
static uint32_t dl_rd(uint32_t w)
{
    uint32_t v = dl_peek(w);
    for (uint32_t k = 2; k <= DL_RD_TRIES; k++) {
        uint32_t x = dl_peek(w);
        if (x == v) {
            if (k > 2) dl_misread(w, k);
            return v;
        }
        v = x;
    }
    dl_misread(w, 0);
    return v;
}

static void write_kernels(void)
{
    for (int i = 0; i < KERNEL_WORDS; i++) {
        dl_wr(KERNEL_WORD + i, cur_kernel_h[i]);
        dl_wr(KERNEL_WORD + KERNEL_WORDS + i, cur_kernel_v[i]);
    }
}

/* ---- borrowing display-list memory ----------------------------------------
   LIST_A, LIST_B and KERNEL_WORD are addresses in the HVS display-list RAM
   that were free when they were measured. They are not mine to keep: other
   code writes that RAM too (the video overlay's CLUT sits at 0x400). So take
   a copy before the first write and put it back when I let the screen go,
   wherever the survey below placed me.

   This was first written believing the RTG driver's mouse pointer plane
   lived in these words. VideoCore.card's source (vc6.c VC6_SetPanning)
   builds the pointer plane and its palette inside the driver's own buddy
   block below 0x300, with the image in Amiga fast RAM - and saving and
   restoring these words did not bring the pointer back, which fits. */
#define DL_BORROW_WORDS  DL_SPAN
static uint32_t dl_backup[DL_BORROW_WORDS];
static uint32_t dl_borrow_first;
static int      dl_borrowed;
uint32_t aga_dl_inuse;      /* words that were non-zero when I took them */
uint32_t aga_dl_free_at;    /* start of the longest free run in the dlist RAM */
uint32_t aga_dl_free_len;   /* and how long it is - both published to agastat */

static void dl_survey(void);    /* defined below; called from dl_borrow */

static void dl_borrow(void)
{
    if (dl_borrowed) return;

    /* What is free NOW, with the desktop up and the driver settled - not what
       was free when somebody measured it once. */
    dl_survey();
    if (aga_dl_free_len >= DL_SPAN) {
        LIST_A      = aga_dl_free_at;
        LIST_B      = LIST_A + LIST_STRIDE;
        KERNEL_WORD = LIST_B + LIST_STRIDE;
    }

    dl_borrow_first = LIST_A;
    uint32_t used = 0;
    for (uint32_t i = 0; i < DL_BORROW_WORDS; i++) {
        /* dl_return writes this back after the game: one read that
           returned another word would stay in the RAM (dl_rd, 2026-10-10) */
        dl_backup[i] = dl_rd(dl_borrow_first + i);
        if (dl_backup[i]) used++;
    }
    aga_dl_inuse = used;        /* 0 = I found somewhere genuinely spare */
    dl_borrowed = 1;
    /* The scaling kernels, now that the words are mine and their old contents
       are saved: always, wherever the lists landed, and whichever look is
       chosen. It used to be written only when a new place was found - before
       the backup, so handing back "restored" my own kernel - and otherwise
       the one written at boot stayed: agaboot ZOOM ... SHARP picked the
       nearest-neighbour kernel and the screen kept the smooth one, the
       useless bastard. */
    write_kernels();
}

/* Where in the display-list RAM is there actually room?

   dlist_words_inuse came back 331 of 331 on the machine: none of the words
   at LIST_A/LIST_B/KERNEL_WORD were free. They start at overlay.c's
   OVL_CLUT_WORD, the video overlay's palette. That is NOT where the mouse
   pointer lives, so relocating stops a real trespass but is not the pointer
   fix; the PTR_* counters exist to find what is.

   This finds the longest run of zero words in the 4096-word (16 KB) display
   list RAM and reports it, so the next build can place my two lists and the
   scaling kernel somewhere the driver is not using - chosen from a
   measurement instead of from a guess that went stale. */
#define DL_RAM_WORDS  4096

/* Never below 0x500. Per VideoCore.card's own source, everything under it is
   spoken for even where it reads as zero: buddyalloc.c manages words
   0-0x2FF (the live lists, the pointer plane and its palette), vc6.c keeps
   Picasso96's palette and the Unicam list at 0x300, and overlay.c puts the
   video overlay's 256-entry CLUT at 0x400. A mostly black palette is a long
   run of zeros, and the survey must not mistake it for room. */
#define DL_SURVEY_FLOOR 0x500

static void dl_survey(void)
{
    uint32_t best_at = 0, best_len = 0, run_at = 0, run = 0;
    /* dl_rd, not one read: a word in use that read as zero once would put
       my lists on it. Two reads a word where one did - about 3600 more
       reads, once per game at the first frame (dl_borrow). */
    for (uint32_t i = DL_SURVEY_FLOOR; i < DL_RAM_WORDS; i++) {
        if (dl_rd(i) == 0) {
            if (!run) run_at = i;
            run++;
            if (run > best_len) { best_len = run; best_at = run_at; }
        } else {
            run = 0;
        }
    }
    /* My own lists are zeroed by dl_return() before the next survey, so a
       region I used last time is free to be chosen again. */
    aga_dl_free_at = best_at;
    aga_dl_free_len = best_len;
}

static void dl_return(void)
{
    if (!dl_borrowed) return;
    for (uint32_t i = 0; i < DL_BORROW_WORDS; i++)
        dl_wr(dl_borrow_first + i, dl_backup[i]);
    dl_borrowed = 0;
}

/* ---- the RTG mouse pointer, read out of the driver's live list -------------
   Diagnostic only - nothing here writes, so one read a word (dl_peek): a
   misread is a wrong counter, never a wrong word in a list. Walks a display
   list the way VideoCore.card vc6.c VC6_SetPanning lays it out, in both its
   unity and its scaled path: the screen plane, then the pointer plane (CTL,
   POS0 at +1, PTR0 at +6, 18 words), then an optional video overlay plane,
   then 0x80000000, one zero word (colour 0, transparent) and the pointer's
   three colours. Fills six counters from `base`: LIST PLANES CTL POS0 IMAGE
   PAL. */
static void ptr_probe(uint32_t list, int base)
{
    if (hvs_gen != 6) return;            /* VC6 layout only; diagnostic */
    uint32_t planes = 0, ctl2 = 0, pos0 = 0, image = 0, pal = 0;
    uint32_t o = list;
    for (int n = 0; n < 8 && o + 6 < DL_RAM_WORDS; n++) {
        uint32_t ctl = dl_peek(o);
        if (ctl == LIST_END) {
            pal = dl_peek(o + 2);           /* terminator, colour 0, colour 1 */
            break;
        }
        uint32_t words = (ctl >> 24) & 0x3f;
        if (!(ctl & CTL_VALID) || words == 0) break;
        if (++planes == 2) {                /* plane 2 is the pointer */
            ctl2  = ctl;
            pos0  = dl_peek(o + 1);
            image = dl_peek(o + 6);
        }
        o += words;
    }
    aga_diag_set(base + 0, list);
    aga_diag_set(base + 1, planes);
    aga_diag_set(base + 2, ctl2);
    aga_diag_set(base + 3, pos0);
    aga_diag_set(base + 4, image);
    aga_diag_set(base + 5, pal);
}

/* ---- mailbox (only used at init, on core 0) ------------------------------- */
#define MBOX_BASE           (ARM_PERIIOBASE + 0xB880)
#define MBOX_READ           (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS         (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE          (*(volatile uint32_t *)(MBOX_BASE + 0x20))
#define MBOX_FULL           0x80000000u
#define MBOX_EMPTY          0x40000000u
/* 128 bytes on a 64-byte boundary: two whole cache lines that nothing else
   lives in. It was aligned(16), so the lines it sits in could be shared with
   neighbouring statics, and a neighbour written by another core between the
   clean and the firmware's answer dirties the line again with the OLD
   request in it - written back on eviction, over the answer. */
static uint32_t req[32] __attribute__((aligned(64)));

static inline uint64_t mbox_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }

/* One property-channel call. Waits for MY answer: the firmware hands back
   the buffer address it was given, and a reply for some other caller's
   buffer (left in the queue, or arriving first) is not mine to stop at. The
   first version was dumb as shit and broke on any channel-8 reply and then
   read a buffer the firmware had not answered yet: the temperature read at
   every sandbox entry and leave came back with a response code of 0, on
   both boards, all along, while the call at video init happened to work.
   Bounded at 100 ms
   so a firmware that never answers cannot hang the caller. */
static int mbox_call(void)
{
    uintptr_t phys = mmu_virt2phys((uintptr_t)req);
    uint32_t addr = (uint32_t)(phys & ~0xFu);
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t end = mbox_now() + f / 10;
    arm_flush_cache((uintptr_t)req, sizeof req);
    while (LE32(MBOX_STATUS) & MBOX_FULL) if (mbox_now() > end) { aga_diag_inc(AGA_D_MBOX_TIMEOUTS); return -1; }
    MBOX_WRITE = LE32(addr | 8);
    for (;;) {
        while (LE32(MBOX_STATUS) & MBOX_EMPTY)
            if (mbox_now() > end) { aga_diag_inc(AGA_D_MBOX_TIMEOUTS); return -1; }
        uint32_t r = LE32(MBOX_READ);
        if ((r & 0xF) == 8 && (r & ~0xFu) == addr) break;
        aga_diag_inc(AGA_D_MBOX_FOREIGN);
    }
    arm_dcache_invalidate((uintptr_t)req, sizeof req);
    return (LE32(req[1]) == 0x80000000u) ? 0 : -1;
}

/* The firmware's view of the SoC: temperature (millidegrees) and the
   GET_THROTTLED bits - 0 under-voltage now, 1 frequency capped now, 2
   throttled now, 3 soft temperature limit now; bits 16-19 the same since
   boot. Independent of the video path, so it works on a Pi 3 too. */
int aga_soc_status(uint32_t *temp_mc, uint32_t *throttled)
{
    int c = 1;
    req[c++] = 0;
    req[c++] = LE32(0x30006); req[c++] = LE32(8); req[c++] = 0;   /* GET_TEMPERATURE id 0 */
    int tpos = c; req[c++] = 0; req[c++] = 0;
    req[c++] = LE32(0x30046); req[c++] = LE32(4); req[c++] = 0;   /* GET_THROTTLED, 0 = do not clear */
    int fpos = c; req[c++] = 0;
    req[c++] = 0;
    req[0] = LE32(c << 2);
    mbox_call();
    aga_diag_inc(AGA_D_SOC_CALLS);
    aga_diag_set(AGA_D_SOC_CODE, LE32(req[1]));   /* $80000000 = the firmware answered */
    if (LE32(req[1]) != 0x80000000u) return -1;
    *temp_mc = LE32(req[tpos + 1]);
    *throttled = LE32(req[fpos]);
    return 0;
}

/* The firmware's own display blanking (property tag 0x40003), the call
   VideoCore.card makes for Picasso96's SetSwitch. A Pi 4's firmware seems to
   ignore it; a Pi 3's honours it, and if the unblank never comes the desktop
   is drawn into a dark output. Sent on the way out on a VideoCore IV. */
static int fw_blank(int blank)
{
    int c = 1;
    req[c++] = 0;
    req[c++] = LE32(0x40003); req[c++] = LE32(4); req[c++] = 0;
    int vpos = c; req[c++] = LE32((uint32_t)(blank ? 1 : 0));
    req[c++] = 0;
    req[0] = LE32(c << 2);
    mbox_call();
    return (LE32(req[1]) == 0x80000000u) ? (int)(LE32(req[vpos]) & 1) : -1;
}

/* ---- state ---------------------------------------------------------------- */
static uint8_t  *fb_va;             /* two frames of AGA_OUT_W x AGA_OUT_H x 4, uncached */
static uintptr_t fb_phys;
static int       fb_page;
static uint32_t  disp_w, disp_h;
static uint32_t  dst_w, dst_h, dst_x, dst_y;
static uint32_t  scale_x, scale_y;  /* 16.16, src/dst */
/* The part of the frame that is shown full screen (agaboot ZOOM). The frame is
   the whole overscan area, 800 x 286 from lores column $50 and line $1A, and a
   standard 640 x 256 screen is only the middle of it: fitted whole, a
   Workbench came out at 63% of a 1080p screen's width with a wide border all
   round, where an Amiga upscaler shows the visible area. The plane simply
   starts further in: same pitch, smaller source, nothing more to render. */
static uint32_t  crop_x, crop_y, crop_w = AGA_OUT_W, crop_h = AGA_OUT_H, crop_off;
static uint32_t  crop_bx, crop_by, crop_bw = AGA_OUT_W, crop_bh = AGA_OUT_H;   /* ...as apply_crop chose it (fit) */
static int       zoom_gen, cur_zoom_gen = -1;
static void fit(void);
static void crop_state(void);       /* the custom crop, AGA_D_CROP (below) */
static void snap_take(void);        /* the last game's picture kept (below) */
static void hsync_restore(void);    /* HDMI sync: the mode's own porch again (below) */
static void hsync_park(void);       /* ...or the porch kept for the next game (below) */
static uint32_t  saved_list;        /* DISPLIST1 as found before I took over */

/* A map of the display-list RAM into the diagnostic log: every run of
   non-zero words as start:length (hex words), plus DISPLIST1. On the Pi 3
   the survey found no free run at all (2026-09-14) and my lists fell back
   onto the compile-time words; this says what the RAM really holds, at
   boot, at the first frame and after the hand-back, with no serial console. */
static void hex_put(char *d, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) { d[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
}
static void dl_map_log(const char *tag)
{
    char line[120];
    int n = 0;
    while (*tag && n < 24) line[n++] = *tag++;
    line[n++] = ' '; line[n++] = 'L';
    hex_put(line + n, reg_rd(REG_DISPLIST1), 4); n += 4; line[n++] = ' ';
    uint32_t run_at = 0, run = 0; int runs = 0;
    for (uint32_t i = 0; i <= DL_RAM_WORDS && runs < 12; i++) {
        uint32_t v = (i < DL_RAM_WORDS) ? dl_peek(i) : 0;
        if (v) { if (!run) run_at = i; run++; }
        else if (run) {
            if (n + 9 >= (int)sizeof line) break;
            hex_put(line + n, run_at, 3); n += 3; line[n++] = ':';
            hex_put(line + n, run, 3); n += 3; line[n++] = ' ';
            run = 0; runs++;
        }
    }
    line[n] = 0;
    aga_diag_log(line);
}
/* eight words of a list, for the same log */
static void dl_words_log(const char *tag, uint32_t at)
{
    char line[120];
    int n = 0;
    while (*tag && n < 24) line[n++] = *tag++;
    line[n++] = ' '; hex_put(line + n, at, 4); n += 4; line[n++] = ':';
    for (int i = 0; i < 8; i++) { line[n++] = ' '; hex_put(line + n, dl_peek(at + i), 8); n += 8; }
    line[n] = 0;
    aga_diag_log(line);
}

/* The watch. hide() arms it; the housekeeper core calls aga_hvs_service()
   on every loop and it takes one sample every 20 ms for three seconds:
   whether DISPLIST1 still holds what I wrote, what DISPLACT1 says the HVS
   is really scanning, and whether the line counter moves at all between
   samples. The last sample dumps the map and the live list's first words. */
static volatile uint64_t watch_next, watch_end;
static uint32_t watch_list_prev, watch_stat_prev;
static int      watch_samples;
static inline uint64_t cnt_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }
static void watch_arm(void)
{
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t now = cnt_now();
    watch_samples = 0;
    watch_list_prev = reg_rd(REG_DISPLIST1);
    watch_stat_prev = reg_rd(REG_DISPSTAT1);
    aga_diag_set(AGA_D_WATCH_SAMPLES, 0);
    aga_diag_set(AGA_D_WATCH_LIST_CHANGES, 0);
    aga_diag_set(AGA_D_WATCH_STAT_MOVES, 0);
    aga_diag_set(AGA_D_WATCH_LACT_FIRST, reg_rd(REG_DISPLACT1));
    watch_end = now + f * 3;
    __atomic_store_n(&watch_next, now + f / 50, __ATOMIC_RELEASE);
}
/* Wait until the HVS has latched DISPLIST1 into DISPLACT1 - which it does
   at a frame start only - so the words of the list it was scanning can be
   rewritten. Bounded: a stalled channel must not stall me too. Returns 1
   when DISPLACT1 reads the list asked for. */
static int wait_latched(uint32_t list, uint32_t timeout_us)
{
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t end = cnt_now() + (f * timeout_us) / 1000000u;
    do {
        if (reg_rd(REG_DISPLACT1) == list) return 1;
    } while (cnt_now() < end);
    return reg_rd(REG_DISPLACT1) == list;
}
static void calib_keep(void);       /* the Position and Crop screens kept on HDMI (below) */
void aga_hvs_service(void)
{
    calib_keep();
    if (!(AGA_PI3_BUILD && AGA_PROBES_ON)) return;   /* Pi 3 test kernels only */
    uint64_t nx = __atomic_load_n(&watch_next, __ATOMIC_ACQUIRE);
    if (!nx) return;
    uint64_t now = cnt_now();
    if (now < nx) return;
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint32_t list = reg_rd(REG_DISPLIST1);
    uint32_t lact = reg_rd(REG_DISPLACT1);
    uint32_t stat = reg_rd(REG_DISPSTAT1);
    if (list != watch_list_prev) aga_diag_inc(AGA_D_WATCH_LIST_CHANGES);
    if ((stat & 0xfff) != (watch_stat_prev & 0xfff)) aga_diag_inc(AGA_D_WATCH_STAT_MOVES);
    watch_list_prev = list; watch_stat_prev = stat;
    watch_samples++;
    aga_diag_set(AGA_D_WATCH_SAMPLES, (uint32_t)watch_samples);
    aga_diag_set(AGA_D_WATCH_LIST_LAST, list);
    aga_diag_set(AGA_D_WATCH_LACT_LAST, lact);
    aga_diag_set(AGA_D_WATCH_STAT_LAST, stat);
    aga_diag_set(AGA_D_WATCH_CTL_LAST, dl_peek(lact));
    aga_diag_set(AGA_D_WATCH_PTR0_LAST, dl_peek(lact + 4));
    if (now >= watch_end) {
        __atomic_store_n(&watch_next, (uint64_t)0, __ATOMIC_RELEASE);
        dl_map_log("[AGA] dlist@+3s");
        dl_words_log("[AGA] DISPLACT1 words", lact);
        if (list != lact) dl_words_log("[AGA] DISPLIST1 words", list);
    } else {
        __atomic_store_n(&watch_next, now + f / 50, __ATOMIC_RELEASE);
    }
}
static uint32_t  cur_list;          /* LIST_A/LIST_B when mine is live, else 0 */
static uint32_t  ptr0_word;         /* dlist word index of my plane's PTR0 */
static int       video_ok;

#define FB_BYTES   (AGA_OUT_W * AGA_OUT_H * 4)
/* a page's place in the reserve: pages 0 and 1 one after the other (a woven
   frame takes them as one), Frame blend's third at AGA_PAGE2_OFF */
static inline uintptr_t page_off(int p) { return p < 2 ? (uintptr_t)p * FB_BYTES : AGA_PAGE2_OFF; }
/* a page not shown now, for the desktop's own screens (crop, Position) */
static inline int page_other(int p) { return p == 0 ? 1 : 0; }
_Static_assert(AGA_HDMI_AUDIO_OFF + AGA_HDMI_AUDIO_SIZE <= AGA_PAGE2_OFF && AGA_PAGE2_OFF + FB_BYTES <= AGA_FB_RESERVE,
               "the third page would reach the sound ring or leave the reserve");

int aga_video_init_at(uint8_t *fbmem_va, uintptr_t fbmem_phys)
{
    fb_va = fbmem_va;
    fb_phys = fbmem_phys;

    /* Which HVS this is. Emu68 tells a Pi 3 from a Pi 4 by the generic timer
       frequency - 19.2 MHz against 54 MHz - and so do I. A VC6 list written
       into a VC4 scaler fucking corrupts it (the first Pi 3 boot of this
       kernel froze on the Emu68 logo that way), so the generation decides
       the RAM offset and every plane word from here on. */
    {
        uint64_t cntfrq;
        __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));
        if (cntfrq <= 20000000) { hvs_gen = 4; dlist_base = DLIST_BASE_VC4; }
        else                    { hvs_gen = 6; dlist_base = DLIST_BASE_VC6; }
        kprintf("[AGA] HVS: VideoCore %s, display lists at %p\n", hvs_gen == 4 ? "IV (Pi 3)" : "VI (Pi 4)", (void *)dlist_base);
    }
    if (hvs_gen == 4 && !AGA_PI3_BUILD) {
        /* The Pi 4 kernel on a Pi 3. Its lists are VC6 lists, which corrupt
           a VC4 scaler, so no AGA picture: games run, the AGA toggle tells
           the user to run the installer again for the Pi 3 kernel. */
        aga_diag_log("[AGA] HVS: Pi 4 kernel on a Pi 3 - no AGA picture, reinstall for the Pi 3");
        return -1;
    }
    if (AGA_PI3_BUILD && AGA_PROBES_ON) dl_map_log("[AGA] dlist@boot");

    /* physical display size */
    int c = 1;
    req[c++] = 0;
    req[c++] = LE32(0x40003); req[c++] = LE32(8); req[c++] = 0;
    int pos = c;
    req[c++] = 0; req[c++] = 0;
    req[c++] = 0;
    req[0] = LE32(c << 2);
    mbox_call();
    disp_w = LE32(req[pos]);
    disp_h = LE32(req[pos + 1]);
    if (disp_w < 320 || disp_h < 200 || disp_w > 4096 || disp_h > 4096) {
        kprintf("[AGA] HVS: implausible display size %dx%d, video disabled\n", disp_w, disp_h);
        return -1;
    }

    fit();

    write_kernels();

    saved_list = reg_rd(REG_DISPLIST1);
    cur_list = 0;
    video_ok = 1;
    crop_state();                       /* bit 0: Custom understood */
    kprintf("[AGA] HVS: display %dx%d, AGA plane %dx%d at %d,%d (scale %08x/%08x), fb %p\n",
            disp_w, disp_h, dst_w, dst_h, dst_x, dst_y, scale_x, scale_y, (void *)fb_phys);
    return 0;
}

int aga_video_init(void)
{
    return video_ok ? 0 : -1;
}

/* The frame about to be shown, from the chipset loop (aga_video_set_frame):
   interlaced and woven by the core - both pages as one frame of 572 rows, its
   own field on rows 2y + frame_row (aga_set_weave) - and PAL or NTSC. What the
   plane reads follows from it (set_source): src_rows rows of src_pitch bytes
   from crop_off on. */
static int frame_woven, frame_row, frame_ntsc;
static int frame_field;             /* the frame shown, as a field of an interlaced screen: 1 long, 2 short,
   0 none */
/* Frame blend's (blend_plane, below) */
static int blend_on;
static int shown_now = -1, shown_before = -1, live_page = -1;
static uintptr_t blend_phys;
static uint32_t blend_at;
/* the CRT look on the GPU's (gpu_show, below): the live list is a GPU one,
   and the picture it shows */
static int cur_gpu;
static int gpu_shown = -1;
static uint32_t src_rows = AGA_OUT_H, src_pitch = AGA_OUT_W * 4;

/* Fit the shown part of the frame into the display. Scale (agaboot RTG Image,
   $597S), RGB2RTG's three:
     0 4:3      the shape a real monitor gives - a hires pixel is 0.52 of a
                line's height (the 14.75 MHz square-pixel rate over the 28.37
                MHz hires one, a line two PAL scanlines tall: RGB2RTG's
                measure) - as large as fits
     1 integer  that shape, with a whole number of display lines for every
                Amiga line: the sharpest, and scanlines come out even
     2 fill     the whole display, shape or not
   and since 1.116 RGB2RTG's shapes (0.75-dev63/64):
     3 5:4      square pixels - a hires pixel half a line's height, PAL or
                NTSC: a PAL screen 5:4 (on a 1280 x 1024 monitor 4x
                exactly), an NTSC one 16:10
     4-7        16:9, 16:10, 21:9, 32:9: the picture stretched to that
                shape, whatever its own, as large as fits
   Up to 1.106 the one fit took hires pixels as square on doubled lines, 4%
   narrower than a monitor.

   An NTSC hires pixel is 0.43 of a line's height (12.27 MHz square pixels
   over the same 28.37 MHz), RGB2RTG's measure too. The TV border ($59Bn
   across, $59Cn down, 0-10%, RGB2RTG's) keeps that much of the display clear
   round the picture, for a TV that cuts the edges of 1080p off: the picture
   fits into what is left, in its shape, centred. */
static int scale_mode;
static int border_x, border_y;
static int scan_pct, lscan_pct;        /* the scanlines' strength, 0-100% (below), for Whole lines */
static int nscan_pct = -1;             /* ...on NTSC frames: -1 the PAL ones' ($517n) */
/* the scanlines' strength for the frame shown: interlaced (woven) ones' own,
   else NTSC ones' when they have their own, else the normal ones' */
static int scan_pct_now(void)
{
    return frame_woven ? lscan_pct : (frame_ntsc && nscan_pct >= 0) ? nscan_pct : scan_pct;
}
/* 0 Even (the default), 1 Crisp, 2 Whole lines, 3 Whole lines (fill),
   4 Beam, 5 Beam (fill) - aga_video_set_scan_shape */
static int scan_shape;
#define SHAPE_WHOLE(s) ((s) == 2 || (s) == 4)   /* a whole number of rows a line, the picture smaller */
#define SHAPE_FILL(s)  ((s) == 3 || (s) == 5)   /* ...the picture larger, the frame's edges cut */
#define SHAPE_BEAM(s)  ((s) == 4 || (s) == 5)
static int scan_top;                   /* 0 the dark row at each line's bottom, 1 its top, 2 CRT fields ($50/$53) */
static volatile int crop_on, calib_on; /* the crop and Position screens (below) */
static void set_source(void);
static void fit(void)
{
    uint32_t aw = disp_w - disp_w * (uint32_t)border_x / 100, ah = disp_h - disp_h * (uint32_t)border_y / 100;
    /* the crop apply_crop chose: Whole lines (fill) cuts from it, afresh at
   every fit */
    crop_x = crop_bx; crop_y = crop_by; crop_w = crop_bw; crop_h = crop_bh;
    if (scale_mode == 2) {
        dst_w = aw;
        dst_h = ah;
    } else {
        static const uint16_t shape[8][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
                                              { 16, 9 }, { 16, 10 }, { 21, 9 }, { 32, 9 } };
        uint64_t sw = (uint64_t)crop_w * (scale_mode == 3 ? 500 : frame_ntsc ? 430 : 520), sh = (uint64_t)crop_h * 1000;   /* the shape */
        if (scale_mode >= 4) { sw = shape[scale_mode][0]; sh = shape[scale_mode][1]; }
        if ((uint64_t)aw * sh <= (uint64_t)ah * sw) { dst_w = aw; dst_h = (uint32_t)(aw * sh / sw); }
        else                                        { dst_h = ah; dst_w = (uint32_t)(ah * sw / sh); }
        if (scale_mode == 1 && dst_h >= crop_h) {
            dst_h = (dst_h / crop_h) * crop_h;
            dst_w = (uint32_t)(dst_h * sw / sh);
        }
    }
    if (dst_w > aw) dst_w = aw;
    if (dst_h > ah) dst_h = ah;
    /* Scanline shape Whole lines (RGB2RTG's VideoCore.card 1.9): while the
       scanlines are on, a whole number of display rows for every Amiga line
       (or woven row) - the height rounded down to n of them, n 2 or more,
       the width by the same factor so the shape is kept, centred below like
       any picture. Every line then gets the same rows, the last one dark: at
       800 x 600, where 4:3 gives a line 2.1 rows, 2 each. Nothing when they
       are whole already (Scale integer) or fewer than 2 (no scanlines). */
    if (SHAPE_WHOLE(scan_shape) && scan_pct_now()) {
        uint32_t lines = (frame_woven && dst_h >= 2 * crop_h) ? 2 * crop_h : crop_h;   /* as set_source shows it */
        uint32_t n = lines ? dst_h / lines : 0;
        if (n >= 2 && n * lines != dst_h) {
            dst_w = (uint32_t)((uint64_t)dst_w * (n * lines) / dst_h);
            dst_h = n * lines;
        }
    }
    /* Whole lines (fill): the next whole number
       of rows a line up instead, and the frame's lines that do not fit cut
       off top and bottom - the overscan frame's 286 lines on 1080 rows show
       270 of them at 4 rows each, the border mostly, where Whole lines makes
       them 3 rows and the picture 858 rows tall. Across, the same step grows
       by as much, and what then passes the display's width is cut at the
       sides. Only while that cuts less than a tenth of the lines: a 640 x
       256 crop would lose 40 lines of the picture itself to go from 4 rows a
       line to 5, so it keeps Whole lines' 4 (1024 rows of 1080), and NTSC's
       640 x 200 its 5. Not on the crop and Position screens, which show the
       frame as it is. */
    if (SHAPE_FILL(scan_shape) && scan_pct_now() && !crop_on && !calib_on) {
        int woven = frame_woven && dst_h >= 2 * crop_h;
        uint32_t lines = woven ? 2 * crop_h : crop_h;
        uint32_t n = lines ? dst_h / lines : 0;
        uint32_t shown = n && n * lines != dst_h ? ah / (n + 1) : lines;   /* the rows that fit, n + 1 each */
        if (woven) shown &= ~1u;                        /* both fields of a frame line */
        if (n >= 2 && n * lines != dst_h && (lines - shown) * 10 >= lines) {
            dst_w = (uint32_t)((uint64_t)dst_w * (n * lines) / dst_h);     /* as Whole lines */
            dst_h = n * lines;
        } else if (n >= 1 && n * lines != dst_h) {
            uint32_t nf = n + 1;
            if (shown > lines) shown = lines;
            uint32_t w = (uint32_t)((uint64_t)dst_w * nf * lines / dst_h);
            uint32_t cw = crop_w;
            if (w > aw) {
                cw = (uint32_t)((uint64_t)crop_w * aw / w) & ~1u;
                w = aw;
            }
            uint32_t ch = woven ? shown / 2 : shown;
            crop_y += (crop_h - ch) / 2;
            crop_x += ((crop_w - cw) / 2) & ~1u;        /* the plane pointer on eight bytes */
            crop_h = ch;
            crop_w = cw;
            dst_h = shown * nf;
            dst_w = w;
        }
    }
    if (!dst_w) dst_w = 1;
    if (!dst_h) dst_h = 1;
    dst_x = (disp_w - dst_w) / 2;
    dst_y = (disp_h - dst_h) / 2;
    scale_x = (0x10000u * crop_w) / dst_w;
    set_source();                       /* the rows read, and so the vertical step */
}

/* 0: the whole 800 x 286 overscan frame, 1: 720 x 272 (what a TV shows),
   2: 640 x 256 (the standard screen, DIWSTRT $2C81: column 114, line 18 of
   the frame). Takes effect on the next frame. Column 114 is measured: six
   games' snapshots put DIW H n at column 2 x (n - 72). Up to 1.106 this said
   98, eight lores pixels early - a standard screen lost its right edge and
   showed as much border on the left - and 720 x 272 sat to match.

   The picture's position moves it on top of the crop, RGB2RTG's way: right
   by pos_x hires pixels means the crop window starts pos_x further LEFT in
   the frame. The window never leaves the frame, so a position one size
   allows the next may clamp - FULL has nowhere to go at all. Steps of two
   hires pixels (one lores pixel), which keep the plane pointer on eight
   bytes; 1.103 held it on sixteen and so started 640 x 256 a lores pixel
   early, at 96. Bloody annoying. */
static const uint16_t zoom_rect[3][4] = { { 0, 0, 800, 286 }, { 74, 10, 720, 272 }, { 114, 18, 640, 256 } };
/* NTSC's frame ends at line 262, row 236 - the rows below it stay black - so
   it has 237 rows, and its standard screen is 640 x 200 in the same place:
   RGB2RTG frames an NTSC picture the same way. */
static const uint16_t zoom_rect_ntsc[3][4] = { { 0, 0, 800, 237 }, { 74, 10, 720, 227 }, { 114, 18, 640, 200 } };
static int zoom_mode, pos_x, pos_y;
/* 3, Custom (1.114): the four edges the user cut off the frame, PAL's and
   NTSC's (aga_crop.h) - $58C0 / $5870 from agaboot's RTG Image. The position
   does not move it: the edges are its place. The crop screen (below) shows
   them whatever the size, while it is up. */
_Static_assert(CROP_FRAME_W == AGA_OUT_W && CROP_ROWS_PAL == AGA_OUT_H, "aga_crop.h has the frame's size");
static uint16_t cut[2][4] = { CROP_DEFAULT_PAL, CROP_DEFAULT_NTSC };
static uint32_t cut_latch;
static volatile int crop_on;
static volatile int last_ok, last_page, last_woven, last_row, last_ntsc;   /* the last game frame shown (snap_take) */
static int snap_ok, snap_ntsc;
static uint32_t snap_us;            /* what the last copy took, for AGA_D_CROP */
/* 4, Auto (1.114, RGB2RTG's Crop: Auto): the game's own display window,
   followed while it runs (aga_crop.h crop_auto_track; the chipset loop hands
   the core's over once a frame, aga_video_auto_window). Until the game shows
   one, 720 x 272. The position does not move it; with Fill the window fills
   the display. Forgotten when the sandbox is left: the next game starts
   afresh. */
static struct crop_auto autow = { -1, 0, 0, 0, 0, 0, 0, 0, 0 };
static void apply_crop(void)
{
    if (zoom_mode == 3 || crop_on) {
        int x, y, w, h;
        crop_cut_rect(cut[frame_ntsc ? 1 : 0], frame_ntsc, &x, &y, &w, &h);
        crop_x = (uint32_t)x; crop_y = (uint32_t)y; crop_w = (uint32_t)w; crop_h = (uint32_t)h;
    } else if (zoom_mode == 4 && autow.x0 >= 0) {
        int rows = frame_ntsc ? 237 : AGA_OUT_H, y0 = autow.y0, y1 = autow.y1;
        if (y1 > rows - 1) y1 = rows - 1;       /* a PAL window, an NTSC frame since */
        if (y0 > rows - 64) y0 = rows - 64;
        if (y1 < y0 + 63) y1 = y0 + 63;
        int cx, cw;                             /* never narrower than the standard screen */
        crop_auto_span(autow.x0, autow.x1, &cx, &cw);
        crop_x = (uint32_t)cx; crop_w = (uint32_t)cw;
        crop_y = (uint32_t)y0; crop_h = (uint32_t)(y1 - y0 + 1);
    } else {
        int sz = zoom_mode > 2 ? 1 : zoom_mode, px = zoom_mode > 2 ? 0 : pos_x, py = zoom_mode > 2 ? 0 : pos_y;
        const uint16_t *z = (frame_ntsc ? zoom_rect_ntsc : zoom_rect)[sz];
        int rows = frame_ntsc ? 237 : AGA_OUT_H;
        int cx = (int)z[0] - px, cy = (int)z[1] - py;
        if (cx > AGA_OUT_W - (int)z[2]) cx = AGA_OUT_W - (int)z[2];
        if (cx < 0) cx = 0;
        if (cy > rows - (int)z[3]) cy = rows - (int)z[3];
        if (cy < 0) cy = 0;
        crop_x = (uint32_t)cx & ~1u; crop_y = (uint32_t)cy; crop_w = z[2]; crop_h = z[3];
    }
    crop_bx = crop_x; crop_by = crop_y; crop_bw = crop_w; crop_bh = crop_h;
    if (disp_w) fit();
    else set_source();
    zoom_gen++;
    crop_state();
}

/* The rows the plane reads. A woven frame is shown whole - both fields, twice
   the rows, a display row or more for each - when the picture is tall enough
   on the display. Shrinking it would need line buffer memory this plane does
   not have (build_list: this is the only plane), so a smaller picture shows
   only the frame's own field, every other row of the woven one, as it looked
   before the flicker fixer. */
static int shown_weave;
/* Sharp's kernels for the steps the plane has now. From set_source, which
   every change of the geometry passes through - a crop, a scale, a border,
   Auto's window, a woven frame - and a kernel is made again only when its
   step changed. While the list is mine the words go to the HVS at once (it
   reads them every line: at worst one frame shows a mix); dl_borrow writes
   them when a game takes over. */
static void sharp_fit(void)
{
    if (cur_look != 0) return;
    const uint32_t *h = kernel_nearest, *v = kernel_nearest;
    int made = 0;
    /* a kernel that fails its own check (sharp_kernel_ok) is not used:
       nearest instead, uneven pixels but no dark rows */
    static int bad_logged;
    if (sharp_uneven(scale_x)) {
        if (area_step_h != scale_x) { sharp_area_kernel(scale_x, kernel_area_h); area_step_h = scale_x; made = 1; }
        if (sharp_kernel_ok(kernel_area_h)) h = kernel_area_h;
        else if (!bad_logged++) aga_diag_log("[AGA] HVS: Sharp's area kernel failed its check - nearest instead");
    }
    if (sharp_uneven(scale_y)) {
        if (area_step_v != scale_y) { sharp_area_kernel(scale_y, kernel_area_v); area_step_v = scale_y; made = 1; }
        if (sharp_kernel_ok(kernel_area_v)) v = kernel_area_v;
        else if (!bad_logged++) aga_diag_log("[AGA] HVS: Sharp's area kernel failed its check - nearest instead");
    }
    if (made || h != cur_kernel_h || v != cur_kernel_v) {
        cur_kernel_h = h;
        cur_kernel_v = v;
        if (video_ok && dl_borrowed) write_kernels();
    }
}

static void set_source(void)
{
    shown_weave = frame_woven && dst_h >= 2 * crop_h;
    int field = frame_woven && !shown_weave;
    src_rows = shown_weave ? 2 * crop_h : crop_h;
    src_pitch = AGA_OUT_W * 4 * (field ? 2 : 1);
    uint32_t row0 = shown_weave ? 2 * crop_y : field ? 2 * crop_y + (uint32_t)frame_row : crop_y;
    crop_off = (row0 * AGA_OUT_W + crop_x) * 4;
    scale_y = (0x10000u * src_rows) / (dst_h ? dst_h : 1);
    sharp_fit();
}

/* Once per frame from the chipset loop (core 3), before aga_video_present:
   what the core made of the frame it just finished. A change of either kind
   is a new geometry; a field shown alone moves every frame, which only the
   plane pointer has to follow. */
void aga_video_set_frame(int woven, int row, int ntsc)
{
    int changed = (woven != frame_woven) || (ntsc != frame_ntsc);
    frame_woven = woven;
    frame_row = row;
    frame_ntsc = ntsc;
    if (changed) apply_crop();
    else if (woven) set_source();
}

/* ...and the same frame as a field (aga_frame_field), for CRT fields (show) */
void aga_video_set_field(int field)
{
    frame_field = field;
}

/* ...and the page the core draws into now, which Frame blend leaves alone */
void aga_video_set_live_page(int page)
{
    live_page = page;
}
static void calib_refresh(void);   /* the Position screen, below */

void aga_video_set_zoom(int mode)
{
    if (mode < 0 || mode > 4) mode = 0;
    zoom_mode = mode;
    apply_crop();
}

/* $58C0 + E5(d): five more bits of an edge; $5870 + E3(s): it into edge
   s & 3 of PAL (s < 4) or NTSC (aga_crop.h). From the 68k's trap on core 0,
   like every control word. */
void aga_video_cut_data(int d) { cut_latch = ((cut_latch << 5) | ((uint32_t)d & 31u)) & 0x3FFu; }
void aga_video_cut_store(int slot)
{
    uint32_t v = cut_latch;
    int ntsc = (slot >> 2) & 1;
    cut_latch = 0;
    cut[ntsc][slot & 3] = (uint16_t)(v > CROP_FRAME_W ? CROP_FRAME_W : v);
    if ((zoom_mode == 3 || crop_on) && ntsc == (frame_ntsc ? 1 : 0)) {
        apply_crop();
        calib_refresh();
    }
}

/* Once a frame from the chipset loop (core 3), after aga_video_set_frame:
   the display window the core saw on the frame's lines with a picture */
void aga_video_auto_window(int ok, int x0, int x1, int y0, int y1)
{
    if (!crop_auto_track(&autow, frame_ntsc ? 237 : AGA_OUT_H, ok, x0, x1, y0, y1)) return;
    aga_diag_set(AGA_D_AUTO_X, (uint32_t)autow.x0 << 16 | (uint32_t)(autow.x1 - autow.x0));
    aga_diag_set(AGA_D_AUTO_Y, (uint32_t)autow.y0 << 16 | (uint32_t)(autow.y1 - autow.y0 + 1) | (frame_ntsc ? 0x8000u : 0));
    if (zoom_mode == 4 && !crop_on) apply_crop();
}

/* $57nn / $56nn: right / down, in hires pixels / lines, signed */
void aga_video_set_pos_x(int x) { pos_x = x; apply_crop(); calib_refresh(); }
void aga_video_set_pos_y(int y) { pos_y = y; apply_crop(); calib_refresh(); }

/* Sharp (nearest neighbour), smooth, or TV. The kernels are read by the HVS
   every line, so writing them takes effect at once - at worst one frame
   shows a mix of the two. */
void aga_video_set_look(int look)
{
    if (look < 0 || look > 2) look = 1;
    cur_look = look;
    cur_kernel_v = look == 0 ? kernel_nearest : kernel_words;
    cur_kernel_h = look == 0 ? kernel_nearest : look == 2 ? kernel_tv : kernel_words;
    sharp_fit();                        /* Sharp: the area kernels where the steps are uneven */
    /* only while the words are mine: on the desktop they belong to the RTG
       driver, and dl_borrow writes the chosen kernels when a game takes over */
    if (video_ok && dl_borrowed) write_kernels();
}
void aga_video_set_sharp(int on) { aga_video_set_look(on ? 0 : 1); }

/* $597S: scale, 0 4:3 / 1 integer / 2 fill (fit, above) */
void aga_video_set_scale(int mode)
{
    scale_mode = (mode >= 0 && mode <= 7) ? mode : 0;
    if (disp_w) fit();
    zoom_gen++;
    calib_refresh();
}

/* $59Bn / $59Cn: the TV border across / down, 0-10% (fit, above) */
void aga_video_set_border(int down, int pct)
{
    pct = pct < 0 ? 0 : pct > 10 ? 10 : pct;
    if (down) border_y = pct; else border_x = pct;
    if (disp_w) fit();
    zoom_gen++;
    calib_refresh();
}

/* Scanlines ($598L, agaboot RTG Image), RGB2RTG's: a second plane over the
   picture, one row per display line, black where an Amiga line ends and clear
   elsewhere, SCAN_W pixels wide and stretched across by the scaler. The HVS
   blends it in as it scans out; nothing is done per frame. Levels as a
   Framethrower's: off, 25, 50, 75, 100% dark. Only where every Amiga line gets
   two display rows or more; even with Scale integer, and with 4:3 or fill
   some come out a row thicker than others. The image sits in the frame
   reserve behind the two pages - uncached, as the HVS reads it. That is the
   whole trick: no per-frame work at all. */
#define SCAN_OFF   0x200000u
#define SCAN_W     16
_Static_assert(2 * FB_BYTES <= SCAN_OFF, "the scanline image would overlap the pages");
#define CTL_SCL(s) ((((uint32_t)(s)) << 5) | (((uint32_t)(s)) << 8))   /* both planes' scaling mode */
#define SCL_H_PPF_V_NONE 4
static const uint8_t scan_alpha[5] = { 0, 64, 128, 192, 255 };
static uint32_t mask_rows, mask_src, mask_level, mask_shape;   /* what the image in memory was written for */

/* The last game's picture for the crop screen, above the scanline image and
   below the HDMI sound ring. Each frame shown is marked (last_*, present);
   it is copied here (snap_take) while its page still holds it: at the first
   frame without a picture after it - the core draws the next one over it,
   and a WHDLoad game quitting turns the display off before it goes - when
   the sandbox is left with it on screen, or when a Position or crop screen
   is about to draw over the pages. One page's copy, uncached to uncached:
   AGA_D_CROP keeps what it took. */
#define SNAP_OFF   0x280000u
_Static_assert(SCAN_OFF + 4096u * SCAN_W * 4 <= SNAP_OFF, "the scanline image for a 4096-line display would reach the snapshot");
_Static_assert(SNAP_OFF + FB_BYTES <= AGA_HDMI_AUDIO_OFF, "the snapshot would reach the HDMI sound ring");

/* The CRT mask ($510n, agaboot RTG Image), RGB2RTG's (VideoCore.card 1.7 and
   1.8): an aperture grille's dark columns as a Trinitron or a PVM shows them -
   one in every three display columns, six on a display taller than 1440
   lines - as dark as the scanline levels. Darkening only: the HVS lays a
   plane over the picture and cannot part its colours as a CRT's phosphors
   do. An unscaled plane over the picture with a pitch of 0, so the HVS reads
   the same row of memory for every line: the grille costs one row, between
   the scanline image and the snapshot. With the scanlines on as well the two
   darkenings multiply, as they do on a CRT - RGB2RTG's user: "much better
   than only scanlines". Pitch 0 checked on a Pi 4 and a tester's Pi 3. */
#define MASK_OFF   0x240000u
#define MASK_W_MAX 4096u
_Static_assert(SCAN_OFF + 4096u * SCAN_W * 4 <= MASK_OFF, "the CRT mask's row would overlap the scanline image");
_Static_assert(MASK_OFF + MASK_W_MAX * 4 <= SNAP_OFF, "the CRT mask's row would reach the snapshot");
static int grille_level;
static uint32_t grille_w, grille_h, grille_lv;   /* what the row in memory was written for */

void aga_video_set_mask(int level)
{
    grille_level = (level >= 0 && level <= 4) ? level : 0;
    zoom_gen++;
    calib_refresh();
}

/* The grille's row for this geometry, written when it is not already; 0 =
   none now. Its columns count from the picture's left edge. */
static int grille_ready(void)
{
    if (!grille_level || dst_w > MASK_W_MAX || !dst_w) return 0;
    if (grille_w == dst_w && grille_h == disp_h && grille_lv == (uint32_t)grille_level) return 1;
    uint32_t *row = (uint32_t *)(fb_va + MASK_OFF);
    uint32_t a = (uint32_t)scan_alpha[grille_level] << 24;     /* black; bytes A,R,G,B like the frames */
    uint32_t period = disp_h > 1440 ? 6 : 3, dark = period / 3;
    for (uint32_t x = 0; x < dst_w; x++)
        row[x] = (x % period) >= period - dark ? a : 0;
    grille_w = dst_w; grille_h = disp_h; grille_lv = (uint32_t)grille_level;
    return 1;
}

/* The mask's plane at word o, over the picture: unscaled (no scaler, no
   kernel), every line from the one row. Returns the word after it. */
#define CTL_UNITY_VC4 (1UL << 4)
#define CTL_UNITY_VC6 (1UL << 15)
static uint32_t mask_plane(uint32_t o)
{
    uint32_t start = o++;                        /* its control word last, like the picture's */
    if (hvs_gen == 4) {
        dl_wr(o++, VC4_POS0(dst_x, dst_y));
        dl_wr(o++, (dst_w & 0xfff) | ((dst_h & 0xfff) << 16));          /* POS2: size, alpha per pixel */
    } else {
        dl_wr(o++, POS0_X(dst_x) | POS0_Y(dst_y));
        dl_wr(o++, POS2_ALPHA(0xfff));                                   /* alpha per pixel */
        dl_wr(o++, POS2_H(dst_h) | POS2_W(dst_w));
    }
    dl_wr(o++, 0xdeadbeefUL);                                            /* POS3 ctx (HVS) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)(fb_phys + MASK_OFF));
    dl_wr(o++, 0xdeadbeefUL);                                            /* PTRCTX0 (HVS) */
    dl_wr(o++, 0);                                                       /* pitch 0: every line the same row */
    dl_wr(start, CTL_VALID | CTL_WORDS(o - start) | (hvs_gen == 4 ? CTL_UNITY_VC4 : CTL_UNITY_VC6)
                 | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND | CTL_FORMAT(FMT_RGBA8888)
                 | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA));
    return o;
}

/* $598L: 0 off, 1-4 = 25-100%, as up to 1.118; $50/$53 below give any
   percent */
void aga_video_set_scanlines(int level)
{
    scan_pct = (level >= 0 && level <= 4) ? 25 * level : 0;
    apply_crop();                       /* Whole lines fits the picture to them */
    calib_refresh();
}

/* $59En: scanlines for an interlaced screen shown woven, RGB2RTG's
   "Interlaced": its rows are lines of their own, so a dark row goes after
   each of them, where the display is tall enough - off (0) suits most */
void aga_video_set_laced_scanlines(int level)
{
    lscan_pct = (level >= 0 && level <= 4) ? 25 * level : 0;
    apply_crop();
}

/* $517n: scanlines of their own for NTSC frames (1.118, RGB2RTG
   0.76-dev85, EMM #2199) - a CRT shows NTSC's lines stronger: the beam is as
   wide, the lines are further apart (a 1084S about 40% dark against PAL's
   27%). n 0 = as PAL's, 1-5 = off, 25 ... 100%. Each frame shown picks its
   level by its own standard (aga_video_set_frame), and a change of standard
   writes the image again for its other line count anyway, so a game that
   switches gets the other lines from the frame it switches at. */
void aga_video_set_ntsc_scanlines(int v)
{
    nscan_pct = (v >= 1 && v <= 5) ? 25 * (v - 1) : -1;
    apply_crop();
    calib_refresh();
}

/* The scanlines in 1% steps (1.119): $50xx
   picks what the next $53xx sets, each with seven bits in bits 0 and 2-7
   (bit 1 and bit 15 clear, so a kernel before 1.119 puts them in the core's
   aga_ctrl harmlessly, as the words since 1.116 do). agaboot sends the 25%
   words first, for such a kernel, and these after them.
     0 the scanlines, 1 interlaced (woven) screens', 2 NTSC screens' - 0-100%,
       NTSC's above 100 as PAL's
     3 the position: 0 the dark row at each line's bottom, 1 at its top,
       2 CRT fields - Bottom, an interlaced screen's short field half a line
       lower (field_shift)
     4 Frame blend: 0 off, 1 on (blend_plane)
     5 Curved screen 0 flat, 1-9; 6 Glow 0 off, 1-100%; 7 Afterglow 0 off,
       1-3 Light, Medium, Strong - a Pi 4's GPU (aga_v3d.c, gpu_begin) */
static int scan_sel;
void aga_video_scan_select(int what)
{
    scan_sel = what;
}
void aga_video_scan_value(int v)
{
    int pct = v > 100 ? 100 : v < 0 ? 0 : v;
    switch (scan_sel) {
    case 0: scan_pct = pct; break;
    case 1: lscan_pct = pct; break;
    case 2: nscan_pct = v > 100 ? -1 : pct; break;
    case 3: scan_top = (v >= 0 && v <= 2) ? v : 0; break;
    case 4:
        if ((v == 1) == blend_on) return;
        blend_on = v == 1;
        shown_now = shown_before = -1;
        aga_glue_set_pages(blend_on ? 3 : 2);   /* the core's pages from its next frame end */
        break;
    case 5: case 6: case 7:
        aga_v3d_set(scan_sel - 5, v);
        break;
    default: return;
    }
    apply_crop();
    calib_refresh();
}

/* $511n: the scanlines' shape, RGB2RTG's (VideoCore.card 1.6 and 1.9):
     0 Even         every Amiga line as dark, also where its display rows are
                    no whole number (4:3 and Fill: 286 lines on 1080 rows are
                    3.78 each) - the dark gap one row tall at each line's end,
                    a row as dark as the part of a gap it covers (the default)
     1 Crisp        a whole dark row at the end of each line, some lines a row
                    taller than others (up to 1.115 the only one)
     2 Whole lines  the picture fitted to a whole number of rows a line (fit),
                    a little smaller, its shape kept: clean at low resolutions
   and since 1.119:
     3 Whole lines (fill)  the next whole number up, the frame's edges cut
                    instead (fit): every line the same at full height
     4 Beam         Whole lines' fit, each line's rows dark towards its edges
                    as a CRT's beam fades: a cosine, its edge rows as dark as
                    the strength, its middle clear
     5 Beam (fill)  the same on Whole lines (fill)'s fit
   A kernel before 1.119 takes 3-5 as Even. */
void aga_video_set_scan_shape(int shape)
{
    scan_shape = (shape >= 0 && shape <= 5) ? shape : 0;
    apply_crop();
    calib_refresh();
}

/* The image for this geometry, written when it is not already; 0 = none now.
   The lines are the rows the plane reads: an Amiga line each, or a woven
   row each for an interlaced screen. */
/* Beam's profile: (1 + cos(2 pi j / 64)) / 2 of 255 for j 0-32, half a line
   from its edge to its middle (the other half mirrors it) */
static const uint8_t beam_w[33] = {
    255, 254, 253, 250, 245, 240, 234, 226, 218, 208, 198, 188, 176, 165, 152, 140,
    128, 115, 103, 90, 79, 67, 57, 47, 37, 29, 21, 15, 10, 5, 2, 1, 0 };
static uint8_t scan_row[4096];         /* scan_ready's darkness for each display row */
static int scan_ready(void)
{
    int pct = scan_pct_now();
    uint32_t lines = src_rows;
    uint32_t shape = (uint32_t)scan_shape | (uint32_t)scan_top << 4;
    if (!pct || dst_h < 2 * lines || dst_h > sizeof scan_row) return 0;
    if ((uint64_t)SCAN_OFF + (uint64_t)dst_h * SCAN_W * 4 > SNAP_OFF) return 0;   /* the crop screen's snapshot is above */
    if (mask_rows == dst_h && mask_src == lines && mask_level == (uint32_t)pct && mask_shape == shape)
        return 1;
    uint32_t alpha = ((uint32_t)pct * 255 + 50) / 100;      /* black; bytes A,R,G,B like the frames */
    for (uint32_t r = 0; r < dst_h; r++) {
        if (scan_shape == 0) {
            /* Even, counted in 1/dst_h of a line: row r covers [r * lines,
              r * lines + lines), the gap of the line it starts in the last
              `lines` of that line's dst_h - with a whole number of rows a
              line, Crisp's rows exactly */
            uint32_t lo = r * lines, hi = lo + lines;
            uint32_t end = (lo / dst_h + 1) * dst_h, gap = end - lines;
            uint32_t s0 = lo > gap ? lo : gap, e0 = hi < end ? hi : end;
            scan_row[r] = (uint8_t)(e0 > s0 ? (alpha * (e0 - s0) + lines / 2) / lines : 0);
        } else {
            scan_row[r] = 0;
        }
    }
    if (scan_shape != 0) {
        /* line by line, its rows as the scaler shows them (a row belongs to
          the line its middle falls in) */
        for (uint32_t r = 0; r < dst_h; ) {
            uint32_t k = ((2 * r + 1) * lines) / (2 * dst_h), r0 = r;
            while (r < dst_h && ((2 * r + 1) * lines) / (2 * dst_h) == k) r++;
            uint32_t h = r - r0;
            if (SHAPE_BEAM(scan_shape) && h >= 3) {
                /* the cosine at each row's middle, (2i + 1) / 2h of the
                  line, its edge rows at the full strength */
                uint32_t e = beam_w[(32 + h / 2) / h];
                for (uint32_t i = 0; i < h; i++) {
                    uint32_t j = ((2 * i + 1) * 32 + h / 2) / h;
                    if (j > 32) j = 64 - j;
                    scan_row[r0 + i] = (uint8_t)(e ? alpha * beam_w[j] / e : alpha);
                }
            } else {
                /* Crisp: the line's last row - as dark for the line as one
                  row is for a line of average height, so a 3-row line of
                  4:3's 3.78 is no darker than a 4-row one (the same row at
                  every whole step); what passes 255 goes on the row above */
                uint32_t want = (alpha * h * lines + dst_h / 2) / dst_h;
                scan_row[r - 1] = (uint8_t)(want > 255 ? 255 : want);
                if (want > 255 && h >= 2) scan_row[r - 2] = (uint8_t)(want - 255 > 255 ? 255 : want - 255);
            }
        }
    }
    /* Top: the image upside down - every gap at its line's start (CRT fields
       keeps Bottom's, moved with the picture instead: show) */
    uint32_t *m = (uint32_t *)(fb_va + SCAN_OFF);
    for (uint32_t r = 0; r < dst_h; r++) {
        uint32_t v = (uint32_t)scan_row[scan_top == 1 ? dst_h - 1 - r : r] << 24;
        for (int x = 0; x < SCAN_W; x++) *m++ = v;
    }
    mask_rows = dst_h; mask_src = lines; mask_level = (uint32_t)pct; mask_shape = shape;
    return 1;
}

/* The scanline plane at word o, over the picture, scaled across only (no
   line buffer needed). Returns the word after it. */
static int cur_shift;                  /* CRT fields (build_list) */
static uint32_t scan_pos0_word;
static uint32_t pos0_word(int dy);
static uint32_t scan_plane(uint32_t o)
{
    uint32_t start = o++;                        /* its control word last, like the picture's */
    scan_pos0_word = o;                          /* moved with the picture at each field (show) */
    if (hvs_gen == 4) {
        dl_wr(o++, pos0_word(cur_shift));
        dl_wr(o++, VC4_POS1(dst_w, dst_h));
        dl_wr(o++, (SCAN_W & 0xfff) | ((dst_h & 0xfff) << 16));        /* POS2: source size, alpha per pixel */
    } else {
        dl_wr(o++, pos0_word(cur_shift));
        dl_wr(o++, POS2_ALPHA(0xfff));                                   /* alpha per pixel */
        dl_wr(o++, POS1_H(dst_h) | POS1_W(dst_w));
        dl_wr(o++, POS2_H(dst_h) | POS2_W(SCAN_W));
    }
    dl_wr(o++, 0xdeadbeefUL);                                            /* POS3 ctx (HVS) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)(fb_phys + SCAN_OFF));
    dl_wr(o++, 0xdeadbeefUL);                                            /* PTRCTX0 (HVS) */
    dl_wr(o++, SCAN_W * 4);                                              /* pitch */
    dl_wr(o++, ((((uint32_t)SCAN_W << 16) / dst_w) << 8) | PPF_SCALER | PPF_PHASE);   /* across */
    for (int i = 0; i < 4; i++) dl_wr(o++, KERNEL_WORD);                /* any: the rows are flat */
    dl_wr(start, CTL_VALID | CTL_WORDS(o - start) | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
                 | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA)
                 | CTL_SCL(SCL_H_PPF_V_NONE));
    return o;
}

/* The picture check (1.114, the Pi 3's dark HDMI picture with lines):
   what build_list wrote into the plane, word by word, mine = 0 for the
   HVS's own words and the frame pointer a flip changes. hvs_check reads the
   live list back against it, and the scaling kernels too. */
#define BL_MAX 24
static uint32_t bl_want[BL_MAX], bl_ctl, bl_n, bl_block;
static uint8_t  bl_mine[BL_MAX];

/* CRT fields (Scanline position 2): the short
   field of an interlaced screen shown a field at a time (Flicker fixer off)
   half a line lower - the picture and its scanlines together - as a CRT
   draws it, the long one where it is: the two fields fill each other's gaps,
   flicker and all. Down where the display has room below the picture, else
   the long field half a line up; nothing for a picture that fills the
   height. build_list places the planes, show moves them at each field;
   cur_shift is the shift the live list has, scan_pos0_word the scanline
   plane's POS0 word (0 = none), both above scan_plane. */
static int field_shift(void)
{
    if (scan_top != 2 || frame_woven || !frame_field || !scan_pct_now() || !src_rows) return 0;
    uint32_t half = (dst_h + src_rows) / (2 * src_rows);   /* half a line, rounded */
    if (!half) return 0;
    if (dst_y + dst_h + half <= disp_h) return frame_field == 2 ? (int)half : 0;
    if (dst_y >= half) return frame_field == 1 ? -(int)half : 0;
    return 0;
}
static uint32_t pos0_word(int dy)
{
    uint32_t y = (uint32_t)((int)dst_y + dy);
    return hvs_gen == 4 ? VC4_POS0(dst_x, y) : (POS0_X(dst_x) | POS0_Y(y));
}
static uint32_t hvs_rebuilds;

/* Frame blend (1.119, after RGB2RTG 0.78-dev31/32, EMM #2296): the frame
   before the last mixed half and half into it as the HVS scans out - a copy
   of the picture's plane right over it, its fixed alpha half, pointed at that
   frame's page. A game's 50 Hz tricks come out steady, as on a CRT: Agony's
   sky, two gradients in turn; Apidya's scroll, a pixel every other frame. No
   time on any core, the Pi 3 too. While it is on the core goes round three
   pages (aga_set_pages): the page under the copy is the one the core drew
   before the last, never the one it draws into. The copy has a line buffer
   of its own: RGB2RTG's first one shared the picture's, two planes scaled
   down the same lines in one buffer, and the blend came out a jittery
   fucking mess.
   Hidden - alpha 0, the plane kept - for a woven frame, steady already, and
   until there is a frame before (a new run of frames, a screen of the
   desktop's own). blend_on the setting; shown_now/shown_before the pages of
   the last two different frames shown; live_page the one the core draws
   into now; blend_phys this show's frame before, 0 hidden; blend_at the live
   list's copy (its control word), 0 none - declared with the frame's state. */
#define BLEND_LBM  (8u * AGA_OUT_W)    /* line buffer words: 8 a source pixel, after the picture's */
static uint32_t blend_alpha(void) { return blend_phys ? (hvs_gen == 4 ? 0x80u : 0x800u) : 0u; }
static uint32_t blend_pos0(int dy)
{
    uint32_t w = pos0_word(dy);
    return hvs_gen == 4 ? (w & 0x00ffffffu) | (blend_alpha() << 24) : w;   /* VC4: the alpha rides in POS0 */
}
/* the copy's words that change with the frame: POS0 (VC4's alpha, CRT
   fields' shift), VC6's alpha word, PTR0 */
static void blend_flip(uint32_t at, int dy)
{
    dl_wr(at + 1, blend_pos0(dy));
    if (hvs_gen != 4) dl_wr(at + 2, POS2_ALPHA_FIXED | POS2_ALPHA(blend_alpha()));
    if (blend_phys) dl_wr(at + (hvs_gen == 4 ? 5 : 6), 0xc0000000UL | (uint32_t)(blend_phys + crop_off));
}
static uint32_t blend_plane(uint32_t o, uint32_t ctl, uintptr_t frame_phys)
{
    uint32_t start = o++;
    if (hvs_gen == 4) {
        dl_wr(o++, blend_pos0(cur_shift));
        dl_wr(o++, VC4_POS1(dst_w, dst_h));
        dl_wr(o++, (crop_w & 0xfff) | ((src_rows & 0xfff) << 16) | (1UL << 30));   /* POS2: alpha FIXED, POS0's */
    } else {
        dl_wr(o++, pos0_word(cur_shift));
        dl_wr(o++, POS2_ALPHA_FIXED | POS2_ALPHA(blend_alpha()));
        dl_wr(o++, POS1_H(dst_h) | POS1_W(dst_w));
        dl_wr(o++, POS2_H(src_rows) | POS2_W(crop_w));
    }
    dl_wr(o++, 0xdeadbeefUL);                                            /* POS3 ctx (HVS) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)((blend_phys ? blend_phys : frame_phys) + crop_off));
    dl_wr(o++, 0xdeadbeefUL);                                            /* PTRCTX0 (HVS) */
    dl_wr(o++, src_pitch);
    dl_wr(o++, BLEND_LBM);
    dl_wr(o++, (scale_x << 8) | PPF_SCALER | PPF_PHASE);
    dl_wr(o++, (scale_y << 8) | PPF_SCALER | PPF_PHASE);
    dl_wr(o++, 0);
    dl_wr(o++, KERNEL_WORD);
    dl_wr(o++, KERNEL_WORD + KERNEL_WORDS);
    dl_wr(o++, KERNEL_WORD);
    dl_wr(o++, KERNEL_WORD + KERNEL_WORDS);
    dl_wr(start, (ctl & ~CTL_WORDS(0x3f)) | CTL_WORDS(o - start));       /* the picture's, its own length */
    blend_at = start;
    return o;
}

/* Build my display list in the block that is not live: one scaled plane,
   Frame blend's copy over it, the scanlines' over them when they are on, and
   the CRT mask's over all. */
static void build_list(uint32_t block, uintptr_t frame_phys)
{
    uint32_t o = block + 1;
    #define BLW(v, mine) do { uint32_t v_ = (v); uint32_t i_ = o - block - 1; \
        if (i_ < BL_MAX) { bl_want[i_] = v_; bl_mine[i_] = (mine); } dl_wr(o++, v_); } while (0)
    cur_shift = field_shift();                                       /* CRT fields (above) */
    scan_pos0_word = 0;
    if (hvs_gen == 4) {
        BLW(pos0_word(cur_shift), 1);                                 /* POS0: x, y, alpha */
        BLW(VC4_POS1(dst_w, dst_h), 1);                               /* scaled destination size */
        BLW(VC4_POS2(crop_w, src_rows), 1);                           /* source size, alpha mode */
    } else {
        BLW(pos0_word(cur_shift), 1);                                    /* POS0 */
        BLW(POS2_ALPHA_FIXED | POS2_ALPHA(0xfff), 1);                    /* POS1 (alpha, VC6) */
        BLW(POS1_H(dst_h) | POS1_W(dst_w), 1);                           /* scaled destination size */
        BLW(POS2_H(src_rows) | POS2_W(crop_w), 1);                       /* source size */
    }
    BLW(0xdeadbeefUL, 0);                                            /* POS3 ctx (HVS) */
    ptr0_word = o;
    BLW(0xc0000000UL | (uint32_t)(frame_phys + crop_off), 0);        /* PTR0: the shown part */
    BLW(0xdeadbeefUL, 0);                                            /* PTRCTX0 (HVS) */
    BLW(src_pitch, 1);                                               /* pitch: a field alone skips rows */
    BLW(0, 1);                                                       /* LBM base: this is the only plane */
    BLW((scale_x << 8) | PPF_SCALER | PPF_PHASE, 1);                 /* PPF0 horizontal */
    BLW((scale_y << 8) | PPF_SCALER | PPF_PHASE, 1);                 /* PPF1 vertical */
    BLW(0, 0);                                                       /* scratch (HVS) */
    BLW(KERNEL_WORD, 1);                            /* kernels: HPPF plane 0 (across) */
    BLW(KERNEL_WORD + KERNEL_WORDS, 1);             /* VPPF plane 0 (down) */
    BLW(KERNEL_WORD, 1);                            /* plane 1's, unused by RGB */
    BLW(KERNEL_WORD + KERNEL_WORDS, 1);
    #undef BLW
    bl_n = o - block - 1;
    bl_block = block;
    bl_ctl = CTL_VALID | CTL_WORDS(o - block) | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
           | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA);
    dl_wr(block, bl_ctl);
    blend_at = 0;
    if (blend_on && !frame_woven) o = blend_plane(o, bl_ctl, frame_phys);
    if (scan_ready()) o = scan_plane(o);
    if (grille_ready()) o = mask_plane(o);
    dl_wr(o++, LIST_END);
    hvs_rebuilds++;
}


/* Length of a plane list in words (0 if it does not look like one). */
static uint32_t list_len(uint32_t start)
{
    uint32_t n = 0;
    for (;;) {
        uint32_t ctl = dl_rd(start + n);
        if (ctl == LIST_END) return n;
        if (!(ctl & CTL_VALID)) return 0;
        uint32_t words = (ctl >> 24) & 0x3f;
        if (words == 0 || n + words > 0x70) return 0;
        n += words;
    }
}

/* Geometry of the first plane of a list (the RTG screen): where the screen's
   pixel (0,0) lands on the display and its scale (16.16 display px per
   screen px). */
static void base_plane_geometry(uint32_t start, int *ox, int *oy, uint32_t *sx, uint32_t *sy, uint32_t *modew)
{
    uint32_t ctl = dl_rd(start);
    uint32_t pos0 = dl_rd(start + 1);
    *sx = *sy = 0x10000; *modew = disp_w;
    if (hvs_gen == 4) {
        /* VC4: POS0 = x | y<<12 | alpha; unity lists go POS0, POS2, scaled ones
           POS0, POS1(dst), POS2(src), and the unity flag is bit 4 */
        *ox = pos0 & 0xfff; *oy = (pos0 >> 12) & 0xfff;
        if (ctl & VC4_CTL_UNITY) {
            *modew = dl_rd(start + 2) & 0xfff;
        } else {
            uint32_t dst = dl_rd(start + 2), src = dl_rd(start + 3);
            uint32_t dw = dst & 0xffff, dh = (dst >> 16) & 0xffff;
            uint32_t sw = src & 0xfff, sh = (src >> 16) & 0xfff;
            if (sw && sh && dw && dh) { *sx = (dw << 16) / sw; *sy = (dh << 16) / sh; *modew = sw; }
        }
        return;
    }
    *ox = pos0 & 0x2fff; *oy = (pos0 >> 16) & 0x2fff;
    if (ctl & (1UL << 15)) {                         /* UNITY: POS0, POS1(alpha), POS2(size) */
        uint32_t pos2 = dl_rd(start + 3);
        *modew = pos2 & 0x3fff;
    } else {                                         /* scaled: POS0, POS1(alpha), POS1(dst), POS2(src) */
        uint32_t dst = dl_rd(start + 3), src = dl_rd(start + 4);
        uint32_t dw = dst & 0xffff, dh = (dst >> 16) & 0xffff;
        uint32_t sw = src & 0x3fff, sh = (src >> 16) & 0x3fff;
        if (sw && sh && dw && dh) { *sx = (dw << 16) / sw; *sy = (dh << 16) / sh; *modew = sw; }
    }
}

/* Windowed: the whole frame in the window - of a woven frame its own field */
static uint32_t win_off(void) { return frame_woven ? (uint32_t)frame_row * AGA_OUT_W * 4 : 0; }

/* Windowed: copy the live RTG list, insert my plane after its first plane
   (so the mouse pointer stays on top), scaled into the window rectangle. The
   copy is read with dl_rd: it is the list on screen until the geometry
   changes - a page flip rewrites PTR0 alone - so one word that read as
   another would stay for the whole game. */
static void build_windowed_list(uint32_t block, uint32_t base, uintptr_t frame_phys, int wx, int wy, int ww, int wh)
{
    uint32_t n = list_len(base);
    int ox, oy; uint32_t sx, sy, modew;
    base_plane_geometry(base, &ox, &oy, &sx, &sy, &modew);
    uint32_t first = (dl_rd(base) >> 24) & 0x3f;
    /* window rectangle in display pixels */
    int dx = ox + (int)(((int64_t)wx * sx) >> 16);
    int dy = oy + (int)(((int64_t)wy * sy) >> 16);
    int dw = (int)(((int64_t)ww * sx) >> 16);
    int dh = (int)(((int64_t)wh * sy) >> 16);
    if (dw < 16) dw = 16;
    if (dh < 16) dh = 16;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx + dw > (int)disp_w) dw = (int)disp_w - dx;
    if (dy + dh > (int)disp_h) dh = (int)disp_h - dy;
    if (dw < 16 || dh < 16) { build_list(block, frame_phys); return; }
    uint32_t scx = (0x10000u * AGA_OUT_W) / (uint32_t)dw;
    uint32_t scy = (0x10000u * AGA_OUT_H) / (uint32_t)dh;

    uint32_t o = block;
    for (uint32_t i = 0; i < first; i++) dl_wr(o++, dl_rd(base + i));      /* RTG screen plane */
    uint32_t plane = o++;
    if (hvs_gen == 4) {
        dl_wr(o++, VC4_POS0(dx, dy));
        dl_wr(o++, VC4_POS1(dw, dh));
        dl_wr(o++, VC4_POS2(AGA_OUT_W, AGA_OUT_H));
    } else {
        dl_wr(o++, POS0_X(dx) | POS0_Y(dy));
        dl_wr(o++, POS2_ALPHA_FIXED | POS2_ALPHA(0xfff));
        dl_wr(o++, POS1_H(dh) | POS1_W(dw));
        dl_wr(o++, POS2_H(AGA_OUT_H) | POS2_W(AGA_OUT_W));
    }
    dl_wr(o++, 0xdeadbeefUL);
    ptr0_word = o;
    /* a woven frame: its own field, every other row (win_off, present) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)(frame_phys + win_off()));
    dl_wr(o++, 0xdeadbeefUL);
    dl_wr(o++, AGA_OUT_W * 4 * (frame_woven ? 2 : 1));
    dl_wr(o++, 8 * modew + 1024);                       /* LBM: after the RTG plane and its mouse pointer */
    dl_wr(o++, (scx << 8) | PPF_SCALER | PPF_PHASE);
    dl_wr(o++, (scy << 8) | PPF_SCALER | PPF_PHASE);
    dl_wr(o++, 0);
    dl_wr(o++, KERNEL_WORD); dl_wr(o++, KERNEL_WORD + KERNEL_WORDS);    /* across, down (as build_list) */
    dl_wr(o++, KERNEL_WORD); dl_wr(o++, KERNEL_WORD + KERNEL_WORDS);
    dl_wr(plane, CTL_VALID | CTL_WORDS(o - plane) | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
               | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA));
    for (uint32_t i = first; i < n; i++) dl_wr(o++, dl_rd(base + i));     /* mouse pointer, overlays */
    dl_wr(o++, LIST_END);
}

static int win_mode, win_x, win_y, win_w, win_h;
static int cur_is_windowed, cur_x, cur_y, cur_w, cur_h;
static int cur_black;       /* the live list of mine is the empty one (RGB port only) */

static void show(uintptr_t frame_phys)
{
#ifdef FRF_VIRTUAL_AGA
    {   /* note the first AGA frame on the boot log (it keeps running until
           aga.splash seconds after the 68k starts, start_rpi64.c) */
        static int said;
        if (!said) { said = 1; kprintf("[AGA] first AGA frame on HDMI\n"); }
    }
#endif
    uint32_t live = reg_rd(REG_DISPLIST1);
    int same_geometry = cur_is_windowed == win_mode && cur_x == win_x && cur_y == win_y && cur_w == win_w && cur_h == win_h
                        && cur_zoom_gen == zoom_gen;
    if (cur_list && live == cur_list && same_geometry && !cur_black && !cur_gpu) {
        /* page flip only; the windowed list shows the whole frame */
        dl_wr(ptr0_word, 0xc0000000UL | (uint32_t)(frame_phys + (cur_is_windowed ? win_off() : crop_off)));
        int sh = cur_is_windowed ? 0 : field_shift();
        if (sh != cur_shift) {
            /* CRT fields: this field half a line lower or not, the picture's
              plane and the scanlines' moved together, and the checker's
              copy of the word with them (hvs_check) */
            uint32_t w = pos0_word(sh);
            dl_wr(cur_list + 1, w);
            bl_want[0] = w;
            if (scan_pos0_word) dl_wr(scan_pos0_word, w);
            cur_shift = sh;
        }
        if (blend_at) blend_flip(blend_at, sh);                   /* Frame blend: the frame before */
        return;
    }
    /* someone else's list is live: remember it so I can hand it back / build
   on it */
    if (live != LIST_A && live != LIST_B) saved_list = live;
    if (hvs_gen == 4) {                       /* the VC6 pointer probe's counters are free here */
        aga_diag_set(AGA_D_PTR_IN_LIST, saved_list);
        aga_diag_set(AGA_D_PTR_IN_PLANES, list_len(saved_list));
        aga_diag_set(AGA_D_PTR_IN_CTL, dl_peek(saved_list));
        aga_diag_set(AGA_D_PTR_IN_POS0, dl_peek(saved_list + 1));
        aga_diag_set(AGA_D_PTR_IN_IMAGE, dl_peek(saved_list + 4));
    }
    if (AGA_PROBES_ON && !cur_list) ptr_probe(saved_list, AGA_D_PTR_IN_LIST);   /* the pointer as I found it */
    if (AGA_PI3_BUILD && AGA_PROBES_ON && !dl_borrowed) dl_map_log("[AGA] dlist@show");
    dl_borrow();            /* before the first write into words I do not own */
    uint32_t block = (cur_list == LIST_A) ? LIST_B : LIST_A;
    /* two rebuilds inside one display frame would write into the list the
       HVS is still scanning; wait for it to move to the current one first */
    if (AGA_PI3_BUILD && cur_list && reg_rd(REG_DISPLACT1) == block) wait_latched(cur_list, 25000);
    if (win_mode && list_len(saved_list) > 0) {
        build_windowed_list(block, saved_list, frame_phys, win_x, win_y, win_w, win_h);
        cur_shift = 0;                  /* no CRT fields in a window */
        scan_pos0_word = 0;
        blend_at = 0;                   /* nor Frame blend */
    } else {
        build_list(block, frame_phys);
    }
    reg_wr(REG_DISPLIST1, block);
    cur_list = block;
    cur_black = 0;
    cur_gpu = 0;
    cur_is_windowed = win_mode; cur_x = win_x; cur_y = win_y; cur_w = win_w; cur_h = win_h;
    cur_zoom_gen = zoom_gen;
}

/* RGB port only: HDMI black for the game. My list is then the empty one -
   just the end word - and with no plane in it the HVS shows its background,
   the black the letterbox bars have always been. The same switch as show()
   (the block not live, the Pi 3's latch wait), so hide() hands the driver's
   list back afterwards exactly as it does for a picture. */
static void show_black(void)
{
    uint32_t live = reg_rd(REG_DISPLIST1);
    if (cur_black && cur_list && live == cur_list) return;
    if (live != LIST_A && live != LIST_B) saved_list = live;
    dl_borrow();
    uint32_t block = (cur_list == LIST_A) ? LIST_B : LIST_A;
    if (AGA_PI3_BUILD && cur_list && reg_rd(REG_DISPLACT1) == block) wait_latched(cur_list, 25000);
    dl_wr(block, LIST_END);
    reg_wr(REG_DISPLIST1, block);
    cur_list = block;
    cur_black = 1;
    cur_gpu = 0;
    blend_at = 0;
}

/* ---- the CRT look on the GPU (1.119, aga_v3d.c) ----------------------------------
 * A curved screen, Glow or Afterglow on a Pi 4: the GPU draws each plain
 * frame at its size on HDMI - the scanlines, the CRT mask and Frame blend in
 * it - into one of two pictures, and my list is then a single 1:1 plane of
 * it, flipped to the next as each is drawn. A draw takes 5 to 9 ms after its
 * frame: core 3 sends it at present (gpu_begin) and goes on with the
 * chipset, the line hook sees it through (aga_video_gpu_poll); HDMI sync
 * presents that much earlier (hsync_frame). Until the first picture of a
 * geometry is drawn - a new crop, scale, look or setting, a woven frame, the
 * GPU still at the last one - the display's own look shows, as without the
 * GPU. Sharp and Smooth: TV keeps the display's look. gpu_key: the geometry
 * and look a picture is drawn with (zoom_gen, cur_look) - shown only while
 * they are still the frame's. */
#define ORDER_ABGR 3                        /* memory bytes R,G,B,A: the GPU's pictures (tools/v3d/mkcrt.py) */
static uint32_t gpu_ptr_word;               /* the GPU list's PTR0 */
static int gpu_draw_slot = -1;              /* the picture being drawn, -1 none */
static uint32_t gpu_draw_key, gpu_key;      /* what it is drawn with, and the shown one's */
static int gpu_draw_shift;                  /* CRT fields: the drawn frame's shift */
static uint32_t gpu_tab_key = ~0u;          /* what pass B's tables were made for */
static uint8_t gpu_col_dark[MASK_W_MAX];

static uint32_t gpu_key_now(void) { return (uint32_t)zoom_gen << 2 | (uint32_t)cur_look; }

static int gpu_frame_ok(void)
{
    return hvs_gen == 6 && aga_v3d_wanted() && cur_look != 2 && !frame_woven && !win_mode && !calib_on &&
           !crop_on && dst_w && dst_h;
}

/* pass B's tables for this geometry: each row's scanline darkness (the
   image's, Top's upside down), each column's mask darkness */
static int gpu_tables(void)
{
    uint32_t key = gpu_key_now();
    if (key == gpu_tab_key) return 1;
    const uint8_t *rows = scan_ready() ? scan_row : 0, *cols = 0;
    if (grille_ready()) {
        const uint32_t *m = (const uint32_t *)(fb_va + MASK_OFF);   /* black at alpha: bytes A,R,G,B */
        for (uint32_t x = 0; x < dst_w; x++) gpu_col_dark[x] = (uint8_t)(m[x] >> 24);
        cols = gpu_col_dark;
    }
    if (!aga_v3d_tables(rows, scan_top == 1, cols, dst_w, dst_h, src_rows, scale_y, cur_look == 0 ? 0 : 1))
        return 0;
    gpu_tab_key = key;
    return 1;
}

/* core 3, a plain frame presented on page `page`: its draw sent, into the
   picture not shown, unless the last one is still being drawn */
static void gpu_begin(int page)
{
    if (gpu_draw_slot >= 0 || !gpu_tables()) return;
    struct aga_v3d_frame f;
    f.src = (uint32_t)(fb_phys + page_off(page) + crop_off);
    f.pitch = src_pitch;
    f.prev = blend_phys ? (uint32_t)(blend_phys + crop_off) : 0;      /* Frame blend: the frame before */
    f.sw = crop_w;
    f.sh = src_rows;
    f.ow = dst_w;
    f.oh = dst_h;
    f.stepx = scale_x;
    f.stepy = scale_y;
    f.look = cur_look == 0 ? 0 : 1;
    f.scan_pct = scan_ready() ? scan_pct_now() : 0;
    int slot = gpu_shown == 0 ? 1 : 0;
    if (!aga_v3d_start(&f, slot)) return;
    gpu_draw_slot = slot;
    gpu_draw_key = gpu_key_now();
    gpu_draw_shift = field_shift();
}

/* the GPU's picture as my whole list: one unscaled plane */
static void build_gpu_list(uint32_t block, uint32_t a, uint32_t pitch)
{
    uint32_t o = block, start = o++;
    dl_wr(o++, pos0_word(cur_shift));
    dl_wr(o++, POS2_ALPHA_FIXED | POS2_ALPHA(0xfff));
    dl_wr(o++, POS2_H(dst_h) | POS2_W(dst_w));
    dl_wr(o++, 0xdeadbeefUL);                                            /* POS3 ctx (HVS) */
    gpu_ptr_word = o;
    dl_wr(o++, 0xc0000000UL | a);
    dl_wr(o++, 0xdeadbeefUL);                                            /* PTRCTX0 (HVS) */
    dl_wr(o++, pitch);
    dl_wr(start, CTL_VALID | CTL_WORDS(o - start) | CTL_UNITY_VC6 | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
                 | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(ORDER_ABGR));
    dl_wr(o++, LIST_END);
    blend_at = 0;
    scan_pos0_word = 0;
    bl_block = 0;                       /* hvs_check: no picture plane of mine to check */
    hvs_rebuilds++;
}

/* a drawn picture shown: its plane flipped to it, or a GPU list built */
static void gpu_show(int slot, int shift)
{
    uint32_t live = reg_rd(REG_DISPLIST1), pitch, a = aga_v3d_slot(slot, &pitch, dst_w);
    if (cur_gpu && live == cur_list && gpu_key == gpu_draw_key) {
        dl_wr(gpu_ptr_word, 0xc0000000UL | a);
        if (shift != cur_shift) { dl_wr(cur_list + 1, pos0_word(shift)); cur_shift = shift; }
        gpu_shown = slot;
        return;
    }
    if (live != LIST_A && live != LIST_B) saved_list = live;
    dl_borrow();
    uint32_t block = (cur_list == LIST_A) ? LIST_B : LIST_A;
    cur_shift = shift;
    build_gpu_list(block, a, pitch);
    reg_wr(REG_DISPLIST1, block);
    cur_list = block;
    cur_black = 0;
    cur_gpu = 1;
    cur_is_windowed = win_mode; cur_x = win_x; cur_y = win_y; cur_w = win_w; cur_h = win_h;
    cur_zoom_gen = zoom_gen;
    gpu_key = gpu_draw_key;
    gpu_shown = slot;
}

/* core 3: from the line hook every few lines, and at present - the draw seen
   through; shown when it is still for the frame's geometry, and while the
   pages are mine (not after the sandbox has gone) */
void aga_video_gpu_poll(void)
{
    if (gpu_draw_slot < 0) return;
    int s, r = aga_v3d_poll(&s);
    if (!r) return;
    gpu_draw_slot = -1;
    if (r < 0 || !cur_list || gpu_draw_key != gpu_key_now() || !gpu_frame_ok()) return;
    gpu_show(s, gpu_draw_shift);
}

static void hide(void)
{
    if (!cur_list) return;
    uint32_t live = reg_rd(REG_DISPLIST1);
    /* Hand the driver's list back whenever mine was the one shown. This used
       to be conditional on the register reading back my address, and a Pi 3
       stayed black as hell after the first game it ever displayed
       (2026-09-14) - a read that does not match must not leave the HVS on a
       plane whose frame is about to stop. Only a live list that is neither
       of mine (the driver re-panned in the meantime) is left alone. */
    if (live == cur_list || live == LIST_A || live == LIST_B) reg_wr(REG_DISPLIST1, saved_list);
    aga_diag_log(live == cur_list ? "[AGA] HVS: handed the driver's list back" : "[AGA] HVS: live list was not ours at hide");
    if (hvs_gen == 4) {
        aga_diag_set(AGA_D_PTR_OUT_LIST, live);
        aga_diag_set(AGA_D_PTR_OUT_PLANES, list_len(saved_list));
        aga_diag_set(AGA_D_PTR_OUT_CTL, dl_peek(saved_list));
        aga_diag_set(AGA_D_PTR_OUT_POS0, dl_peek(saved_list + 1));
        aga_diag_set(AGA_D_PTR_OUT_IMAGE, dl_peek(saved_list + 4));
        int r = fw_blank(0);
        aga_diag_set(AGA_D_PTR_NOW_LIST, reg_rd(REG_DISPLIST1));
        aga_diag_set(AGA_D_PTR_NOW_PLANES, (uint32_t)r);
        aga_diag_log(r < 0 ? "[AGA] HVS: firmware unblank failed" : "[AGA] HVS: firmware unblank sent");
    }
    cur_list = 0;
    cur_black = 0;
    blend_at = 0;
    shown_now = shown_before = -1;      /* Frame blend: a new run of frames next time */
    cur_gpu = 0;
    gpu_shown = -1;
    /* Only after the HVS is scanning the driver's list again, never while it
       is still walking mine. This used to be a comment and no wait: on the
       Pi 3 the restored words are garbage, the scaler parsed them for the
       rest of the frame, hung, and never reached the frame start where it
       latches the new address - black desktop, line counter frozen. I do not
       write comments where a wait belongs any more. */
    if (AGA_PI3_BUILD) {
        uint64_t t0 = cnt_now();
        int ok = wait_latched(saved_list, 60000);
        uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
        aga_diag_set(AGA_D_WATCH_LATCH_US, (uint32_t)(((cnt_now() - t0) * 1000000u) / f));
        aga_diag_log(ok ? "[AGA] HVS: driver's list latched, ours released" : "[AGA] HVS: driver's list NOT latched in 60 ms");
    }
    dl_return();
    if (AGA_PROBES_ON) ptr_probe(reg_rd(REG_DISPLIST1), AGA_D_PTR_OUT_LIST);      /* the pointer as I left it */
    if (AGA_PI3_BUILD && AGA_PROBES_ON) {
        dl_map_log("[AGA] dlist@hide");
        dl_words_log("[AGA] handed-back words", saved_list);
        watch_arm();
    }
}

/* Sandbox leave / watchdog park. The loop stops presenting, but nothing
   told the HVS to stop SHOWING - so the last frame stayed on top of the
   desktop. Invisible while the last frame was always a hidden one; the moment
   the overlay made it a black frame with digits on it, a machine that had
   left the sandbox cleanly looked wedged, with the counters frozen at
   whatever they read on the way out. */
void aga_video_hide(void)
{
    if (!video_ok) return;
    snap_take();            /* the game's last picture, before the next session draws over the pages */
    crop_auto_reset(&autow);    /* the next game's window is its own */
    hsync_park();               /* HDMI sync: the porch kept for the next game, or the mode's own */
    if (zoom_mode == 4) apply_crop();
    hide();
}

/* Live read, from the diagnostic dump path: whatever the HVS is scanning
   right now, sandbox or not. Run agastat while the pointer is invisible and
   these counters are the state on screen. Reads only. */
void aga_video_probe_pointer(void)
{
    if (video_ok) ptr_probe(reg_rd(REG_DISPLIST1), AGA_D_PTR_NOW_LIST);
}
volatile int aga_overlay_idle = 0;   /* show the overlay on a blank plane while the sandbox idles */

/* Called from the chipset loop (core 3) once per completed frame. */
void aga_video_set_window(int mode, int x, int y, int w, int h)
{
    win_mode = mode; win_x = x; win_y = y; win_w = w; win_h = h;
}

/* ---- debug overlay --------------------------------------------------------
 * When a game wedges, AmigaOS is gone and caffed cannot answer - but the
 * chipset loop keeps presenting frames, so the screen is the only channel
 * left. And every way out of a wedge (Ctrl-Amiga-Amiga included, measured
 * 2026-09-07) resets the Pi and wipes the counters, which is why two Banshee
 * runs in a row told me nothing at all. So: draw the numbers into the frame
 * and photograph them.
 *
 * Seven-segment rather than a font: sixteen glyphs out of a sixteen-byte
 * table and four rectangles, with no font data to carry around.
 */
#define OVL_ROWS 12
static volatile uint32_t ovl_val[OVL_ROWS];
#ifdef AGA_DIAG
static volatile int      ovl_on = 1;      /* diag builds: on, to photograph a wedge */
#else
static volatile int      ovl_on = 0;      /* the build people play: a clean picture */
#endif

void aga_video_overlay(int row, uint32_t v) { if ((unsigned)row < OVL_ROWS) ovl_val[row] = v; }
#ifdef FRF_VIRTUAL_AGA
/* EMU68AGA standalone: the HUD only opens when config.txt asks for it
   (dtoverlay=diagnostic,hud -> /emu68/diag/frf-hud, see aga_glue_init) */
static volatile int      ovl_allowed = 0;
void aga_video_overlay_allow(int on)        { ovl_allowed = on ? 1 : 0; if (!ovl_allowed) ovl_on = 0; }
void aga_video_overlay_enable(int on)       { ovl_on = (ovl_allowed && on) ? 1 : 0; }
#else
void aga_video_overlay_enable(int on)       { ovl_on = on; }
#endif

/* bit 0..6 = segments a,b,c,d,e,f,g */
static const uint8_t seg7[16] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,
    0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71,
};

#define DW 10          /* digit cell width  */
#define DH 18          /* digit cell height */
#define DT 2           /* segment thickness */

static void ovl_fill(uint32_t *dst, int x, int y, int w, int h, uint32_t col)
{
    if (x < 0 || y < 0 || x + w > AGA_OUT_W || y + h > AGA_OUT_H) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            dst[(y + j) * AGA_OUT_W + (x + i)] = col;      /* native, like the core's pixels */
}

static void ovl_digit(uint32_t *dst, int x, int y, int d, uint32_t col)
{
    int s = seg7[d & 15], half = (DH - 3 * DT) / 2;
    if (s & 0x01) ovl_fill(dst, x + DT,      y,                     DW - 2 * DT, DT,   col); /* a */
    if (s & 0x02) ovl_fill(dst, x + DW - DT, y + DT,                DT,          half, col); /* b */
    if (s & 0x04) ovl_fill(dst, x + DW - DT, y + 2 * DT + half,     DT,          half, col); /* c */
    if (s & 0x08) ovl_fill(dst, x + DT,      y + 2 * DT + 2 * half, DW - 2 * DT, DT,   col); /* d */
    if (s & 0x10) ovl_fill(dst, x,           y + 2 * DT + half,     DT,          half, col); /* e */
    if (s & 0x20) ovl_fill(dst, x,           y + DT,                DT,          half, col); /* f */
    if (s & 0x40) ovl_fill(dst, x + DT,      y + DT + half,         DW - 2 * DT, DT,   col); /* g */
}

static void ovl_draw(uint32_t *dst)
{
    const int x0 = 8, y0 = 8, pitch = DH + 6;
    ovl_fill(dst, x0 - 4, y0 - 4, 8 * (DW + 2) + 8, OVL_ROWS * pitch + 8, 0xFF000000u);
    for (int r = 0; r < OVL_ROWS; r++) {
        uint32_t v = ovl_val[r];
        /* row 0 is the one that matters most, so make it stand out */
        uint32_t col = 0xFF000000u | (r == 0 ? 0x00FF6060u : 0x0000FF00u);
        for (int d = 0; d < 8; d++)
            ovl_digit(dst, x0 + d * (DW + 2), y0 + r * pitch, (v >> ((7 - d) * 4)) & 0xF, col);
    }
}

/* ---- the Position screen ------------------------------------------------------
 * agaboot POSITION (Chipset menu - RTG Image - Position...) moves the picture
 * over this. RGB2RTG opens a native screen for it, because there the Amiga's
 * own output is what HDMI shows; here the desktop's native screens never reach
 * HDMI at all, so the kernel draws the pattern into the frame itself, exactly
 * where a standard PAL screen lands (DIWSTRT $2C81: 640 x 256 from column 114,
 * line 18), and shows it through the current size, look and position. The rest
 * of the frame - the overscan a game may use - is dark blue, so its edges show
 * too. Grid, border, corner marks and centre as on RGB2RTG's screen; the two
 * offsets, right and down, in the overlay's digits. agaboot keeps the window
 * that takes the keys and the mouse, on the desktop nobody can see meanwhile.
 * Never while the sandbox is up: a game or the system-wide desktop owns the
 * plane then, and a move shows on the live picture instead. */
#define CAL_X0  114
#define CAL_Y0  18
#define CAL_W   640
#define CAL_H   256
static volatile int calib_on;

static void cal_hline(uint32_t *d, int x0, int x1, int y, uint32_t c) { ovl_fill(d, x0, y, x1 - x0 + 1, 1, c); }
static void cal_vline(uint32_t *d, int x, int y0, int y1, uint32_t c) { ovl_fill(d, x, y0, 1, y1 - y0 + 1, c); }

/* sign and up to three digits, right-aligned in four cells */
static void cal_number(uint32_t *d, int x, int y, int v, uint32_t col)
{
    int a = v < 0 ? -v : v, half = (DH - 3 * DT) / 2;
    if (a > 999) a = 999;
    if (v) {
        ovl_fill(d, x + DT, y + DT + half, DW - 2 * DT, DT, col);                   /* minus: segment g */
        if (v > 0) ovl_fill(d, x + DW / 2 - DT / 2, y + DT + half / 2, DT, half + DT, col);   /* plus: and an upright */
    }
    for (int i = 3; i >= 1; i--) {
        ovl_digit(d, x + i * (DW + 2), y, a % 10, col);
        a /= 10;
        if (!a) break;
    }
}

static void calib_pattern(uint32_t *d)
{
    const uint32_t outside = 0xFF101830u, black = 0xFF000000u, grid = 0xFF404050u,
                   white = 0xFFFFFFFFu, orange = 0xFFFF9900u;
    for (int i = 0; i < AGA_OUT_W * AGA_OUT_H; i++) d[i] = outside;
    ovl_fill(d, CAL_X0, CAL_Y0, CAL_W, CAL_H, black);
    for (int x = 32; x < CAL_W; x += 32) cal_vline(d, CAL_X0 + x, CAL_Y0, CAL_Y0 + CAL_H - 1, grid);
    for (int y = 32; y < CAL_H; y += 32) cal_hline(d, CAL_X0, CAL_X0 + CAL_W - 1, CAL_Y0 + y, grid);
    for (int k = 0; k < 2; k++) {                  /* the standard area's border, two pixels */
        cal_hline(d, CAL_X0 + k, CAL_X0 + CAL_W - 1 - k, CAL_Y0 + k, white);
        cal_hline(d, CAL_X0 + k, CAL_X0 + CAL_W - 1 - k, CAL_Y0 + CAL_H - 1 - k, white);
        cal_vline(d, CAL_X0 + k, CAL_Y0 + k, CAL_Y0 + CAL_H - 1 - k, white);
        cal_vline(d, CAL_X0 + CAL_W - 1 - k, CAL_Y0 + k, CAL_Y0 + CAL_H - 1 - k, white);
    }
    for (int k = 0; k < 4; k++) {                  /* corner marks, eight pixels in */
        int cx = CAL_X0 + ((k & 1) ? CAL_W - 9 : 8), cy = CAL_Y0 + ((k & 2) ? CAL_H - 9 : 8);
        int hx = (k & 1) ? cx - 40 : cx, vy = (k & 2) ? cy - 20 : cy;
        cal_hline(d, hx, hx + 40, cy, orange);
        cal_vline(d, cx, vy, vy + 20, orange);
    }
    cal_hline(d, CAL_X0 + 300, CAL_X0 + 340, CAL_Y0 + CAL_H / 2, orange);          /* the centre */
    cal_vline(d, CAL_X0 + 320, CAL_Y0 + CAL_H / 2 - 20, CAL_Y0 + CAL_H / 2 + 20, orange);
}

static void calib_draw(uint32_t *d)
{
    const uint32_t black = 0xFF000000u, white = 0xFFFFFFFFu, orange = 0xFFFF9900u;
    calib_pattern(d);
    /* the offsets, as agaboot says them: an arrow right and its number, an
       arrow down and its number, on black so the grid does not cross them */
    int y = CAL_Y0 + 80, x = CAL_X0 + 320 - 80, mid = y + DH / 2;
    ovl_fill(d, x - 8, y - 6, 176, DH + 12, black);
    cal_hline(d, x, x + 12, mid, white);
    for (int k = 1; k <= 4; k++) { ovl_fill(d, x + 12 - k, mid - k, 1, 1, white); ovl_fill(d, x + 12 - k, mid + k, 1, 1, white); }
    cal_number(d, x + 18, y, pos_x, orange);
    x += 18 + 4 * (DW + 2) + 16;
    cal_vline(d, x + 6, y + 2, y + DH - 3, white);
    for (int k = 1; k <= 4; k++) { ovl_fill(d, x + 6 - k, y + DH - 3 - k, 1, 1, white); ovl_fill(d, x + 6 + k, y + DH - 3 - k, 1, 1, white); }
    cal_number(d, x + 16, y, pos_y, orange);
}

/* Copy the last game frame to SNAP_OFF while the pages still hold it: of a
   woven one - both pages one frame of 572 rows - its own field. 64-bit
   volatile copies, aligned: the pages are uncached. */
static void snap_take(void)
{
    if (!last_ok) return;
    last_ok = 0;
    uint64_t t0 = cnt_now();
    volatile uint64_t *d = (volatile uint64_t *)(fb_va + SNAP_OFF);
    for (int y = 0; y < AGA_OUT_H; y++) {
        uintptr_t row = last_woven ? (uintptr_t)(2 * y + last_row) * AGA_OUT_W * 4
                                   : page_off(last_page) + (uintptr_t)y * AGA_OUT_W * 4;
        const volatile uint64_t *s = (const volatile uint64_t *)(fb_va + row);
        for (int i = 0; i < AGA_OUT_W / 2; i++) *d++ = s[i];
    }
    snap_ok = 1;
    snap_ntsc = last_ntsc;
    uint64_t f;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t us = ((cnt_now() - t0) * 1000000u) / (f ? f : 1);
    snap_us = us > 0xFFFF ? 0xFFFF : (uint32_t)us;
    crop_state();
}

/* ---- the crop screen ----------------------------------------------------------
 * agaboot's Crop... (RTG Image, Size Custom): the last game's picture - or,
 * with none of the standard being cut since the Pi started, the Position
 * screen's pattern - cut to the edges being set, as the game will show. Each
 * edge's number sits by it; the one the keys move is orange, with a bar
 * along it. $5880 + E3(s) puts it up (aga_crop.h) or marks another edge,
 * $5930 ends it as it ends Position. Desktop only, like Position: with the
 * sandbox up agaboot cuts the live picture instead. */
static int crop_std, crop_edge;

/* AGA_D_CROP, for agaboot: bit 0 the kernel cuts Custom edges, bit 1 the
   crop screen shows the last game's picture, bit 2 the frame being cut is
   NTSC, bit 3 there is a last game picture to show, bit 4 it is NTSC; bits
   16-31 the microseconds the last picture's copy took */
static void crop_state(void)
{
    int have = snap_ok || last_ok, hn = last_ok ? last_ntsc : snap_ntsc;
    aga_diag_set(AGA_D_CROP, 1u | ((crop_on && snap_ok && snap_ntsc == crop_std) ? 2u : 0u) | (frame_ntsc ? 4u : 0u)
                             | (have ? 8u : 0u) | ((have && hn) ? 16u : 0u) | (snap_us << 16));
}

/* up to three digits, right-aligned, on black */
static void crop_number(uint32_t *d, int x, int y, int v, uint32_t col)
{
    ovl_fill(d, x - 4, y - 4, 3 * (DW + 2) + 6, DH + 8, 0xFF000000u);
    for (int i = 2; i >= 0; i--) {
        ovl_digit(d, x + i * (DW + 2), y, v % 10, col);
        v /= 10;
        if (!v) break;
    }
}

static void crop_marks(uint32_t *d)
{
    const uint32_t white = 0xFFFFFFFFu, orange = 0xFFFF9900u;
    int x = (int)crop_x, y = (int)crop_y, w = (int)crop_w, h = (int)crop_h, nw = 3 * (DW + 2) - 2;
    int rows = frame_ntsc ? 237 : AGA_OUT_H;
    int v[4]  = { x, y, AGA_OUT_W - x - w, rows - y - h };
    int nx[4] = { x + 10, x + (w - nw) / 2, x + w - 10 - nw, x + (w - nw) / 2 };
    int ny[4] = { y + (h - DH) / 2, y + 10, y + (h - DH) / 2, y + h - 10 - DH };
    switch (crop_edge) {                         /* the bar, inside the edge */
    case CUT_L: ovl_fill(d, x, y, 4, h, orange); break;
    case CUT_T: ovl_fill(d, x, y, w, 2, orange); break;
    case CUT_R: ovl_fill(d, x + w - 4, y, 4, h, orange); break;
    default:    ovl_fill(d, x, y + h - 2, w, 2, orange); break;
    }
    for (int e = 0; e < 4; e++) crop_number(d, nx[e], ny[e], v[e], e == crop_edge ? orange : white);
}

static void crop_show(void)
{
    int p = page_other(fb_page);
    uint32_t *d = (uint32_t *)(fb_va + (uintptr_t)p * FB_BYTES);
    if (snap_ok && snap_ntsc == crop_std) {
        volatile uint64_t *o = (volatile uint64_t *)d;
        const volatile uint64_t *s = (const volatile uint64_t *)(fb_va + SNAP_OFF);
        for (uint32_t i = 0; i < FB_BYTES / 8; i++) o[i] = s[i];
    } else {
        calib_pattern(d);
    }
    frame_woven = 0;                    /* one page, of the standard being cut */
    frame_row = 0;
    frame_ntsc = crop_std;
    apply_crop();
    crop_marks(d);
    show(fb_phys + (uintptr_t)p * FB_BYTES);
    fb_page = p;
}

/* The Position and Crop screens go on HDMI once and stay there while the
   user drags: the sandbox is down, so no frame comes to show them again.
   VideoCore.card points DISPLIST1 at its own lists when it likes - with a
   Framethrower (its switch mode CSI) at every Picasso96 SetSwitch, which a
   tester's A600 got as soon as the mouse moved - and the desktop, or the
   Framethrower's live picture, then stayed on HDMI in the screen's place.
   So the housekeeper core looks every millisecond while one of them is up
   and points the HVS back at my list, keeping the driver's newest list for
   the hand-back (calib_keep, below). keep_lock keeps it out of core 0's way
   while a control word builds or hands back a list. */
static volatile int keep_lock;
static volatile uint32_t keep_count;
static inline void keep_take(void) { while (__atomic_exchange_n(&keep_lock, 1, __ATOMIC_ACQUIRE)) ; }
static inline void keep_give(void) { __atomic_store_n(&keep_lock, 0, __ATOMIC_RELEASE); }

void aga_video_crop_screen(int slot)
{
    if (!video_ok || aga_sandbox_active) return;
    keep_take();
    snap_take();
    calib_on = 0;
    crop_std = (slot >> 2) & 1;
    crop_edge = slot & 3;
    crop_on = 1;
    crop_show();
    keep_give();
}

static void calib_show(void)
{
    int p = page_other(fb_page);
    uint32_t *d = (uint32_t *)(fb_va + (uintptr_t)p * FB_BYTES);
    calib_draw(d);
    show(fb_phys + (uintptr_t)p * FB_BYTES);
    fb_page = p;
}

/* $5931 on, $5930 off. From the abort handler on core 0, like every control
   word; with the sandbox down core 3 does not present, so the pages and the
   lists are mine to use here. */
/* how often calib_keep took HDMI back, into the log as the screen ends
   (core 0) */
static void keep_log(void)
{
    static const char tag[] = "[AGA] HVS: Position/Crop screen taken back from the driver x";
    char line[72];
    uint32_t n = keep_count;
    int i = 0;
    if (!n) return;
    keep_count = 0;
    for (; tag[i]; i++) line[i] = tag[i];
    hex_put(line + i, n > 0xFFFF ? 0xFFFF : n, 4);
    line[i + 4] = 0;
    aga_diag_log(line);
}

void aga_video_calibrate(int on)
{
    if (!video_ok) return;
    keep_take();
    if (!on) {
        if (calib_on || crop_on) {
            int was_crop = crop_on;
            calib_on = 0;
            crop_on = 0;
            if (!aga_sandbox_active) hide();
            if (was_crop) apply_crop(); /* the size's own geometry again */
            keep_log();
        }
    } else if (!aga_sandbox_active) {
        snap_take();                    /* the last game's picture, before the pattern covers it */
        if (crop_on) { crop_on = 0; apply_crop(); }
        calib_on = 1;
        /* the pattern is one PAL page: whatever the last game left the frame
          state at - woven, NTSC - the Position screen is framed as a PAL one */
        if (frame_woven || frame_ntsc) aga_video_set_frame(0, 0, 0);
        calib_show();
    }
    keep_give();
}

static void calib_refresh(void)
{
    if (!video_ok || aga_sandbox_active) return;
    keep_take();
    if (calib_on) calib_show();
    else if (crop_on) crop_show();
    keep_give();
}

/* The housekeeper core, every visit (aga_hvs_service): two loads while
   neither screen is up; with one up, a look at DISPLIST1 every millisecond -
   well inside the 20 ms before the HVS latches it at the next frame - and
   my list back if the driver's is there. Skipped while core 0 is at it. */
static void calib_keep(void)
{
    static uint64_t next;
    if (!(calib_on | crop_on)) return;
    uint64_t now = cnt_now();
    if (now < next) return;
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    next = now + f / 1000u;
    if (__atomic_exchange_n(&keep_lock, 1, __ATOMIC_ACQUIRE)) return;
    if ((calib_on || crop_on) && video_ok && !aga_sandbox_active && cur_list && !cur_black) {
        uint32_t live = reg_rd(REG_DISPLIST1);
        if (live != cur_list) {
            if (live != LIST_A && live != LIST_B) saved_list = live;   /* handed back at the end */
            reg_wr(REG_DISPLIST1, cur_list);
            keep_count++;
        }
    }
    keep_give();
}

/* ---- HDMI sync (1.114, agaboot RTG Image) ----------------------------------------
 * The HDMI frame follows the emulated one (aga_sync.h): at each frame shown,
 * the HVS's line says where in the HDMI frame it was presented, and the
 * vertical front porch of the PixelValve that times HDMI - VERTB's top half -
 * is made a line or two longer to hold that place. RGB2RTG's rules for the
 * writes (its rgb_follow.c): the mode's own porch to 16 lines more, never
 * less; one line at a time; only while the HVS is well inside the picture,
 * since a porch changed while the PixelValve is in it could send its line
 * counter past the end and the display lose sync; the mode's own porch kept
 * from the first look, and the PixelValve looked at again once a second -
 * a new mode (another active height) is learned afresh, and a PixelValve
 * not enabled or not showing the HVS's height is never written. HDMI0's on
 * the Pi 4 (PixelValve 2), the only one on the Pi 3. Off by default ($5891
 * on, $5890 off); the mode's own porch comes back when it is switched off
 * and when the sandbox is left. */
#define PV_BASE      (hvs_gen == 4 ? 0xF2807000UL : 0xF220A000UL)
#define PV_V_CONTROL 0x04
#define PV_VERTA     0x14
#define PV_VERTB     0x18
static volatile int hsync_on;
static int pv_state;                    /* 0 not looked at, 1 good, -1 not to be written */
static int pv_have;                     /* vfp0/vactive0 are the mode's own */
static uint32_t pv_vfp0, pv_vactive0;
static int pv_k;                        /* porch lines over the mode's own, in force */
static uint32_t frame_len;              /* the emulated frame's colour clocks (aga_video_set_frame_len) */
static uint64_t pv_look;                /* the next look at the PixelValve */
static struct hsync hs = { -1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0 };

static uint64_t cnt_freq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f ? f : 1; }

static int pv_check(void)
{
    uint32_t vc = reg_rd(PV_BASE + PV_V_CONTROL), va = reg_rd(PV_BASE + PV_VERTA), vb = reg_rd(PV_BASE + PV_VERTB);
    if (!pv_have || (vb & 0xffff) != pv_vactive0) {        /* the first look, or a new mode */
        pv_vfp0 = vb >> 16;
        pv_vactive0 = vb & 0xffff;
        pv_have = 1;
        pv_k = 0;
        hsync_reset(&hs);
    }
    int32_t dh = (int32_t)disp_h - (int32_t)pv_vactive0;
    uint32_t vtotal = pv_vactive0 + pv_vfp0 + (va >> 16) + (va & 0xffff);
    aga_diag_set(AGA_D_HDMI_SYNC_PV, pv_vfp0 << 16 | pv_vactive0);
    return (vc & 1) && pv_vfp0 >= 1 && pv_vfp0 < 64 && dh >= -2 && dh <= 2 && vtotal > pv_vactive0;
}

static void hsync_report(int phase)
{
    aga_diag_set(AGA_D_HDMI_SYNC, (uint32_t)(hsync_on ? 1 : 0) | (uint32_t)(pv_state == 1 ? 2 : pv_state < 0 ? 4 : 0)
                                  | (uint32_t)(hs.reach + 1) << 4 | (uint32_t)pv_k << 8 | (uint32_t)(phase & 0xffff) << 16);
    aga_diag_set(AGA_D_HDMI_SYNC_NEED, (uint32_t)hs.need16);
    aga_diag_set(AGA_D_HDMI_SYNC_SLIPS, hs.slips);
}

/* the porch to `k` lines over the mode's own, once the HVS is well inside
   the picture - waiting up to a frame for it (wait 0: only if it is now) */
static int pv_write(int k, int wait)
{
    uint64_t end = cnt_now() + cnt_freq() / 40;
    for (;;) {
        uint32_t line = reg_rd(REG_DISPSTAT1) & 0xfff;
        if (line > 100 && line + 200 < pv_vactive0) {
            reg_wr(PV_BASE + PV_VERTB, (pv_vfp0 + (uint32_t)k) << 16 | pv_vactive0);
            pv_k = k;
            return 1;
        }
        if (!wait || cnt_now() >= end) return 0;
    }
}

/* the mode's own porch again (switched off, the sandbox left) */
static void hsync_restore(void)
{
    if (pv_have && pv_state == 1 && pv_k) pv_write(0, 1);
    hs.prev = -1;
    hsync_report(-1);
}

/* The sandbox left (RGB2RTG's PARK, 0.76-dev82, EMM #2196): the porch a
   steady, followed game held stays for the desktop and the next game, which
   - the same kind of game - needs it again and so starts with no HDMI
   timing change at all (on the 50.2 Hz mode that is six lines, walked one a
   frame). Learning, coming in, or out of reach: the mode's own, as before.
   A new HDMI mode is learned afresh (pv_check); HDMI sync switched off puts
   the mode's own back (hsync_restore). */
static void hsync_park(void)
{
    if (pv_have && pv_state == 1 && hs.reach == 1 && hs.steady) {
        hs.prev = -1;
        hsync_report(-1);
        return;
    }
    hsync_restore();
}

/* Once per frame from the chipset loop (core 3), before aga_video_present:
   the frame's length, for HDMI sync's measure (aga_sync.h hsync_len) */
void aga_video_set_frame_len(uint32_t cck)
{
    frame_len = cck;
}

/* each frame shown (core 3, after show) */
static void hsync_frame(void)
{
    if (!hsync_on) return;
    uint64_t now = cnt_now();
    if (!pv_state || now >= pv_look) {
        pv_state = pv_check() ? 1 : -1;
        pv_look = now + cnt_freq();
    }
    if (pv_state != 1) { hsync_report(-1); return; }
    uint32_t line = reg_rd(REG_DISPSTAT1) & 0xfff;
    int phase = line + 2 < pv_vactive0 ? (int)line : -1;
    hsync_len(&hs, frame_len);
    /* the CRT look on the GPU: its picture reaches HDMI a draw after the
       present - 1080 lines in about 19.2 ms at 50 Hz, a millisecond more for
       the polls; 10 ms before the first draw. From the first frame that
       goes to the GPU on, not its first picture: a lead that came in
       mid-game moved HDMI's timing by hundreds of lines in one go, and a
       monitor blanked (the CM4, 10-25). At most 16 lines a frame either
       way. */
    {
        uint32_t need = aga_v3d_need_us();
        int want = gpu_frame_ok() ? (int)(((need ? need : 10000u) + 1000u) * pv_vactive0 / 19200u) : 0;
        hs.lead = want > hs.lead + 16 ? hs.lead + 16 : want < hs.lead - 16 ? hs.lead - 16 : want;
    }
    int want = hsync_step(&hs, phase, (int)pv_vactive0, pv_k);
    if (want > pv_k + 1) want = pv_k + 1;
    if (want < pv_k - 1) want = pv_k - 1;
    if (want != pv_k) pv_write(want, 0);
    hsync_report(phase);
}

/* $5891 on / $5890 off (agaboot RTG Image, HDMI sync) - core 0, the trap */
void aga_video_hdmi_sync(int on)
{
    if (!video_ok) return;
    hsync_on = on ? 1 : 0;
    if (!on) hsync_restore();
    else hsync_report(-1);
}

/* Read the live plane and the kernels back against what was written: a
   word that differs is logged (the first eight a boot) with what it holds
   now. Every 50th frame, core 3, from aga_video_present. It found nothing
   changed when the Pi 3's picture went dark with lines (2026-10-10) - the
   kernel itself had been computed wrong, by a GCC miscompile of the A53
   tuning (see Emu68's CMakeLists) - and stays as the cheap guard it is. */
static void hvs_check(void)
{
    static int logged;
    static uint32_t looks, bad_total;
    static char line[96];
    if (!cur_list || cur_black || cur_is_windowed || bl_block != cur_list) return;
    looks++;
    uint32_t bad = 0;
    for (uint32_t i = 0; i <= bl_n && i < BL_MAX + 1; i++) {
        uint32_t w, want;
        if (i == 0) { w = bl_block; want = bl_ctl; }
        else if (i - 1 < BL_MAX && bl_mine[i - 1]) { w = bl_block + i; want = bl_want[i - 1]; }
        else continue;
        uint32_t v = dl_rd(w);
        if (v == want) continue;
        bad++;
        if (logged < 8) {
            int n = 0;
            for (const char *s = "[AGA] HVS check: word "; *s; ) line[n++] = *s++;
            hex_put(line + n, w, 3); n += 3;
            for (const char *s = " wrote "; *s; ) line[n++] = *s++;
            hex_put(line + n, want, 8); n += 8;
            for (const char *s = " now "; *s; ) line[n++] = *s++;
            hex_put(line + n, v, 8); n += 8;
            line[n] = 0;
            aga_diag_log(line);
            logged++;
        }
    }
    for (int i = 0; i < KERNEL_WORDS; i++) {
        uint32_t kh = dl_rd(KERNEL_WORD + i), kv = dl_rd(KERNEL_WORD + KERNEL_WORDS + i);
        for (int k = 0; k < 2; k++) {
            uint32_t v = k ? kv : kh, want = k ? cur_kernel_v[i] : cur_kernel_h[i];
            if (v == want) continue;
            bad++;
            if (logged < 8) {
                int n = 0;
                for (const char *s = k ? "[AGA] HVS check: kernel V " : "[AGA] HVS check: kernel H "; *s; ) line[n++] = *s++;
                hex_put(line + n, (uint32_t)i, 2); n += 2;
                for (const char *s = " wrote "; *s; ) line[n++] = *s++;
                hex_put(line + n, want, 8); n += 8;
                for (const char *s = " now "; *s; ) line[n++] = *s++;
                hex_put(line + n, v, 8); n += 8;
                line[n] = 0;
                aga_diag_log(line);
                logged++;
            }
        }
    }
    bad_total += bad;
}

void aga_video_present(const uint32_t *frame, int native_active)
{
    if (!video_ok) return;
    calib_on = 0;      /* the sandbox has the pages now: a Position screen left up is over */
    if (crop_on) { crop_on = 0; apply_crop(); }   /* ...and a crop screen */
    /* No native display. Normally I hide the plane and let the RTG desktop
       show through - and that is what the "grey screen" has been all along:
       not my output at all, but P96's screen sitting blank after WHDLoad took
       the machine off AmigaOS. Which also means the overlay never drew,
       because I returned here first, the daft bastard.

       When the sandbox is up, that is exactly the case worth looking at, so
       show a black plane with the numbers on it instead. Outside the sandbox
       nothing changes and the desktop is untouched. */
    /* the RGB port black after 10 frames without a picture (1.119): with
       AGA from boot nothing ends the session when a game quits to the RTG
       Workbench, and its last frame stayed on the RGB monitor. Not at once:
       a game that blanks for a frame between screens would flash it. */
    static int no_picture;
    if (!native_active) {
        if (++no_picture == 10) aga_rgbout_frame(0, 0, -1);
    } else {
        no_picture = 0;
    }
    if (!native_active) {
        snap_take();        /* the frame before this one is still in its page, until the next is drawn */
        hs.prev = -1;       /* HDMI sync: the next frame shown starts a new run of phases */
        /* Default: hide. My display list REPLACES the HVS list, so while
           the plane is up the RTG desktop is not drawn at all - and that hid
           WHDLoad's own splash menu, whose Start button nobody could fucking
           click. The idle overlay is opt-in (agaboot OVERLAY ON, $5F01) for the
           case where a game hangs without ever drawing. */
#ifdef FRF_VIRTUAL_AGA
        /* standalone: no sandbox, the machine IS the virtual chipset */
        if (!(ovl_on && aga_overlay_idle)) { hide(); return; }
#else
        if (!(ovl_on && aga_sandbox_active && aga_overlay_idle)) { hide(); return; }
#endif
        if (frame_woven) aga_video_set_frame(0, 0, frame_ntsc);   /* the digits go on one page */
        /* TRANSPARENT, not black. The plane is set up with alpha mode
           FIXED_NONZERO (POS2_ALPHA_FIXED), so a pixel whose alpha byte is
           zero is not drawn at all and the RTG desktop shows through; the
           overlay box is opaque and stays readable. A black plane hid a
           WHDLoad requester behind the digits and cost a power-cut to find. */
        int p = page_other(fb_page);
        uint32_t *d = (uint32_t *)(fb_va + page_off(p));
        shown_now = shown_before = -1;          /* Frame blend: afresh after it */
        for (int i = 0; i < AGA_OUT_W * AGA_OUT_H; i++) d[i] = 0;
        ovl_draw(d);
        show(fb_phys + (uintptr_t)p * FB_BYTES);
        fb_page = p;
        return;
    }

    /* The core rendered this frame straight into one of my two pages (see
       aga_set_framebuffers): no copy, and the pages are uncached, so no
       flush either. This used to be 900 KB read, 900 KB written and 900 KB
       of cache cleaned per frame - 1.2 ms on a Pi 4 and, on a Pi 3, the
       working set the game's chip RAM traffic evicted. */
    int page = frame == (const uint32_t *)(fb_va + page_off(1)) ? 1
             : frame == (const uint32_t *)(fb_va + page_off(2)) ? 2 : 0;
    uint32_t *dst = (uint32_t *)(fb_va + page_off(page));
    /* the RGB port's picture: the frame, or of a woven one - both pages as
       one, from the first - its own field, every other row */
    const uint32_t *rgb = dst;
    int rgb_pitch = AGA_OUT_W;
    if (frame_woven) {
        page = 0;
        dst = (uint32_t *)fb_va;
        rgb = (const uint32_t *)(fb_va + (uintptr_t)frame_row * AGA_OUT_W * 4);
        rgb_pitch = 2 * AGA_OUT_W;
    }
    /* the crop screen's picture, while nothing draws over it (snap_take) */
    last_page = page;
    last_woven = frame_woven;
    last_row = frame_row;
    last_ntsc = frame_ntsc ? 1 : 0;
    last_ok = 1;
    crop_state();
    if (aga_rgbout_hdmi_off()) {
        /* the game on the RGB port only: the frame still goes to core 1 */
        show_black();
        fb_page = page;
        aga_rgbout_frame(rgb, rgb_pitch, frame_woven ? -1 : page);
        return;
    }
    if (ovl_on && !frame_woven) ovl_draw(dst);     /* the digits assume one page */
    /* Frame blend: the frame shown before this one - a frame core 3 skipped
       shows its page again, and the one before stays - when it is still
       whole: not the page the core draws into, nor a woven frame's */
    if (frame_woven) shown_now = shown_before = -1;
    else if (page != shown_now) { shown_before = shown_now; shown_now = page; }
    blend_phys = blend_on && shown_before >= 0 && shown_before != page && shown_before != live_page
               ? fb_phys + page_off(shown_before) : 0;
    /* the CRT look on the GPU: this frame's draw sent; its picture shows
       once drawn (aga_video_gpu_poll) - meanwhile the GPU's last one, or the
       display's own look while there is none of this geometry */
    aga_video_gpu_poll();
    int gpu = gpu_frame_ok();
    if (gpu) gpu_begin(page);
    if (!gpu || !cur_gpu || gpu_key != gpu_key_now() || reg_rd(REG_DISPLIST1) != cur_list)
        show(fb_phys + page_off(page));
    blend_phys = 0;                                 /* the desktop's own screens: none */
    fb_page = page;
    {
        static uint32_t check_n;
        if (++check_n % 50 == 0) hvs_check();
    }
    hsync_frame();
    aga_rgbout_frame(rgb, rgb_pitch, frame_woven ? -1 : page);   /* and to the RGB port as HAM6, when a game has it on (core 1) */
}

/* ---- mode switch: reboot the Pi, optionally with the firmware's one-shot tryboot flag
   so that tryboot.txt (the AGA configuration) is used for the next boot only ------ */
#ifdef PISTORM_CLASSIC
/* The classic protocol keeps its reset inline in the housekeeper and exports
   no pi_reset(), so do the same watchdog sequence it uses
   (ps_classic_protocol.c, the PIN_RESET branch). */
#define PM_RSTC         ((volatile unsigned int *)(0xf2000000 + 0x0010001c))
#define PM_RSTS         ((volatile unsigned int *)(0xf2000000 + 0x00100020))
#define PM_WDOG         ((volatile unsigned int *)(0xf2000000 + 0x00100024))
#define PM_WDOG_MAGIC   0x5a000000
#define PM_RSTC_FULLRST 0x00000020
static void pi_reset(void)
{
    unsigned int r = LE32(*PM_RSTS);
    r &= ~0xfffffaaau;                       /* boot from partition 0 */
    *PM_RSTS = LE32(PM_WDOG_MAGIC | r);
    *PM_WDOG = LE32(PM_WDOG_MAGIC | 10);
    *PM_RSTC = LE32(PM_WDOG_MAGIC | PM_RSTC_FULLRST);
    for (;;) { }
}
#else
extern void pi_reset(void);
#endif

void aga_reboot(int tryboot)
{
#ifdef FRF_VIRTUAL_AGA
    /* Standalone has no physical reset-line housekeeper to save the previous
       session. Save it explicitly before the Pi watchdog reset. */
    aga_diag_log("[AGA] standalone host reboot: preserving previous session");
    aga_diag_persist();
#endif
    aga_diag_inc(AGA_D_REBOOTS);
    if (tryboot) {
        int c = 1;
        req[c++] = 0;
        req[c++] = LE32(0x38064); req[c++] = LE32(4); req[c++] = 0;   /* SET_REBOOT_FLAGS */
        req[c++] = LE32(1);                                           /* bit 0 = tryboot */
        req[c++] = 0;
        req[0] = LE32(c << 2);
        mbox_call();
        kprintf("[AGA] tryboot flag set, rebooting into AGA mode\n");
    } else {
        kprintf("[AGA] rebooting into ECS mode\n");
    }
    pi_reset();
}
