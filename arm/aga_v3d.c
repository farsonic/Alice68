/* AGA-PISTORM - the CRT look on the Pi 4's 3D GPU, the V3D 4.2 (1.119):
 * a curved screen, Glow, Afterglow and Frame blend drawn by GPU programs
 * between the core's frame and HDMI. The whole fucking idea is RGB2RTG's
 * (0.78-dev2 to dev30, kernel/rgb_v3d.h, docs/V3D-SHADERS.md, Astair86,
 * 2026), the first compute jobs anyone ran on a Pi 4's V3D without Linux;
 * my 32-bit pixel programs in aga_v3d_code.h (tools/v3d/mkcrt.py).
 *
 * The passes, a compute job (CSD) each, the H buffer between the bastards:
 *   G  Glow only: a glow buffer pixel for each 8 x 4 source pixels
 *   A  a work-group a line of the crop: the line scaled across to the
 *      picture's width (Smooth or Sharp), on a curved screen through the
 *      barrel, mixed with the frame before (Frame blend), the glow added
 *   C  Afterglow only: each pixel at least the frame before's, faded
 *   B  a work-group a row of the picture: two lines of H mixed with the
 *      scanlines' weights, times the CRT mask's column - on a curved screen
 *      the line found through the barrel, the scanlines from the beam
 * into one of two pictures in the GPU room (AGA_GPU_RESERVE), which
 * aga_hvs.c shows as a 1:1 plane.
 *
 * What's mine: core 3 runs the chipset itself and must never wait the 5 to 9
 * ms the passes take - RGB2RTG's core 3 only converts frames, and waits.
 * Here aga_v3d_start sends the first pass and aga_v3d_poll, called from the
 * line hook and at each frame, sends the next once one is done: a few
 * register pokes a time. Waking the V3D waits for nothing either: it is
 * woken on core 0 the first time a setting wants it, never at boot.
 *
 * Bring-up, after Linux's bcm2835-power.c and v3d driver (GPL-2.0+), as
 * RGB2RTG does it: the V3D's reset released in the power block (PM_GRAFX
 * bit 6, with the block's password), its two bus bridges opened master
 * before slave; before a job the bloody L2T flushed and the slice caches
 * invalidated, after it the TMU write combiner flushed and the L2T cleaned;
 * the GPU's MMU off, physical addresses. Its clock is the firmware's (500 MHz).
 * The ARM runs big-endian, the GPU reads little-endian: every program word,
 * uniform and table it reads is byte-swapped (RGB2RTG's dev18 lesson).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdint.h>
#include "aga_glue.h"
#include "aga_v3d.h"
#include "aga_v3d_code.h"

extern uintptr_t mmu_virt2phys(uintptr_t virt);
extern void arm_flush_cache(uintptr_t addr, uint32_t len);
extern void *tlsf;
extern void *tlsf_malloc(void *handle, uintptr_t size);

#define V3D_PERI        0xF2000000UL                    /* ARM_PERIIOBASE: physical 0xFE000000 */
#define V3D_HUB         (V3D_PERI + 0xC00000)
#define V3D_CORE0       (V3D_PERI + 0xC04000)
#define V3D_ASB         (V3D_PERI + 0xC11000)           /* the V3D's bus bridges, a BCM2711's little gift */
#define PM_GRAFX        (V3D_PERI + 0x10010C)
#define PM_PASSWORD     0x5A000000u
#define PM_V3DRSTN      (1u << 6)
#define ASB_S_CTRL      0x08
#define ASB_M_CTRL      0x0C
#define ASB_BRDG_ID     0x20                            /* reads "brdg" */
#define ASB_REQ_STOP    (1u << 0)
#define ASB_ACK         (1u << 1)
#define V3D_CTL_IDENT0  0x00                            /* "V3D" and its version: 0x04443356 */
#define V3D_CTL_SLCACTL     0x24
#define V3D_CTL_L2TCACTL    0x30
#define V3D_CTL_L2TFLSTA    0x34
#define V3D_CTL_L2TFLEND    0x38
#define V3D_CSD_STATUS      0x900                       /* completed dispatches 11:4 */
#define V3D_CSD_CFG(i)      (0x904 + 4 * (i))           /* CFG0, written last, starts it */
/* 16 work-groups a supergroup, 16 batches, 16 invocations a work-group, as
   Mesa chooses it (RGB2RTG dev11: one a supergroup put every batch on one QPU) */
#define V3D_ROW_CFG3    (15u << 12 | 0u << 8 | 16)

static inline uint32_t v3d_rd(uintptr_t a) { return __builtin_bswap32(*(volatile uint32_t *)a); }
static inline void v3d_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint64_t v3d_now(void) { uint64_t t; __asm__ volatile("isb; mrs %0, CNTPCT_EL0" : "=r"(t)); return t; }
static inline uint64_t v3d_freq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f; }

/* ---- waking it ------------------------------------------------------------ */
enum { V3D_TRIED = 1, V3D_BRIDGE = 2, V3D_RELEASED = 4, V3D_NO_ACK = 8, V3D_ANSWERS = 16, V3D_MEMORY = 32 };
static volatile int v3d_state;

static int bridge_open(uint32_t reg)
{
    uint32_t v = v3d_rd(V3D_ASB + reg);
    if (v & ASB_REQ_STOP) v3d_wr(V3D_ASB + reg, PM_PASSWORD | (v & 0x00ffffffu & ~ASB_REQ_STOP));
    for (int i = 0; i < 100000; i++)
        if (!(v3d_rd(V3D_ASB + reg) & ASB_ACK)) return 1;
    return 0;
}

/* the programs' rooms (each its uniforms at a fixed place after it), the H
   buffers, the glow buffer: the kernel's heap, below 4 GB, the GPU's alone -
   scrubbed out of the ARM's caches once, so no line is written back over them */
#define UNIF_FLAT   1024                /* code up to 1 KB: the flat passes */
#define UNIF_CURVE  2048                /* curved and blending ones */
#define UNIF_FX     3072                /* Glow's pass A */
_Static_assert(V3D_LINES_CODE_N * 8 <= UNIF_FLAT && V3D_SCREEN_CODE_N * 8 <= UNIF_FLAT &&
               V3D_AFTERGLOW_CODE_N * 8 <= UNIF_FLAT, "a flat V3D program runs into its uniforms");
_Static_assert(V3D_LINES_CURVE_CODE_N * 8 <= UNIF_CURVE && V3D_SCREEN_CURVE_CODE_N * 8 <= UNIF_CURVE &&
               V3D_LINES_BLEND_CODE_N * 8 <= UNIF_CURVE && V3D_LINES_CURVE_BLEND_CODE_N * 8 <= UNIF_CURVE &&
               V3D_GLOW_CODE_N * 8 <= UNIF_CURVE, "a curved or blending V3D program runs into its uniforms");
_Static_assert(V3D_LINES_GLOW_CODE_N * 8 <= UNIF_FX && V3D_LINES_CURVE_GLOW_CODE_N * 8 <= UNIF_FX &&
               V3D_LINES_BLEND_GLOW_CODE_N * 8 <= UNIF_FX && V3D_LINES_CURVE_BLEND_GLOW_CODE_N * 8 <= UNIF_FX,
               "a Glow V3D program runs into its uniforms");
#define H_PITCH     (1920u * 4)         /* the widest fucking picture I draw */
#define H_ROWS      288u                /* the most lines of a crop: a plain frame's 286 */
#define GLOW_COLS   128u                /* 800 / 8 */
#define GLOW_ROWS   72u                 /* 286 / 4 */
#define GLOW_PITCH  (GLOW_COLS * 4)

/* [curved][blend][glow] for pass A, [curved] for pass B */
static uint8_t *pass_a[2][2][2], *pass_b[2], *pass_g, *pass_c;
static uint8_t *hbuf[2], *gbuf;

static uint8_t *heap(uint32_t bytes)
{
    uint8_t *m = tlsf_malloc(tlsf, bytes + 64);
    if (!m || (mmu_virt2phys((uintptr_t)m) >> 32)) return 0;
    m = (uint8_t *)(((uintptr_t)m + 63) & ~(uintptr_t)63);
    arm_flush_cache((uintptr_t)m, bytes);
    return m;
}

static uint8_t *prog(const uint64_t *code, unsigned n, unsigned room)
{
    uint8_t *a = heap(room + 256);
    if (!a) return 0;
    uint64_t *c = (uint64_t *)a;
    for (unsigned i = 0; i < n; i++) c[i] = __builtin_bswap64(code[i]);
    arm_flush_cache((uintptr_t)a, n * 8);
    return a;
}

static uintptr_t gpu_va, gpu_pa;        /* the GPU room: the two pictures */
static uint32_t gpu_bytes;

void aga_v3d_room(uint8_t *va, uintptr_t pa, uint32_t bytes)
{
    gpu_va = (uintptr_t)va;
    gpu_pa = pa;
    gpu_bytes = bytes;
}

/* core 0, the first time a setting wants the GPU: wake the fucker, build
   its programs and buffers. 1 when it can draw. */
static int v3d_wake(void)
{
    if (v3d_state & V3D_MEMORY) return 1;
    if (v3d_state & V3D_TRIED) return 0;            /* tried before, and failed */
    v3d_state = V3D_TRIED;
    if (v3d_freq() <= 20000000 || !gpu_bytes) return 0;    /* a Pi 3 (19.2 MHz timer), or no room */
    if (v3d_rd(V3D_ASB + ASB_BRDG_ID) != 0x62726467u) return 0;
    v3d_state |= V3D_BRIDGE;
    uint32_t g = v3d_rd(PM_GRAFX);
    if (!(g & PM_V3DRSTN)) {
        v3d_wr(PM_GRAFX, PM_PASSWORD | (g & 0x00ffffffu) | PM_V3DRSTN);
        v3d_state |= V3D_RELEASED;
        for (int i = 0; i < 1000; i++) (void)v3d_rd(PM_GRAFX);     /* the reset needs a few clocks, so humour the bastard */
    }
    if (!bridge_open(ASB_M_CTRL) || !bridge_open(ASB_S_CTRL)) {
        v3d_state |= V3D_NO_ACK;
        aga_diag_log("[AGA] GPU: the V3D's bridges did not open - the CRT look stays the display's own");
        return 0;
    }
    if (v3d_rd(V3D_CORE0 + V3D_CTL_IDENT0) != 0x04443356u) {
        aga_diag_log("[AGA] GPU: no V3D 4.2 answers - the CRT look stays the display's own");
        return 0;
    }
    v3d_state |= V3D_ANSWERS;
    static const uint64_t *const ac[2][2][2] = {
        { { v3d_lines_code, v3d_lines_glow_code }, { v3d_lines_blend_code, v3d_lines_blend_glow_code } },
        { { v3d_lines_curve_code, v3d_lines_curve_glow_code },
          { v3d_lines_curve_blend_code, v3d_lines_curve_blend_glow_code } } };
    static const unsigned an[2][2][2] = {
        { { V3D_LINES_CODE_N, V3D_LINES_GLOW_CODE_N }, { V3D_LINES_BLEND_CODE_N, V3D_LINES_BLEND_GLOW_CODE_N } },
        { { V3D_LINES_CURVE_CODE_N, V3D_LINES_CURVE_GLOW_CODE_N },
          { V3D_LINES_CURVE_BLEND_CODE_N, V3D_LINES_CURVE_BLEND_GLOW_CODE_N } } };
    int ok = 1;
    for (int c = 0; c < 2; c++)
        for (int b = 0; b < 2; b++)
            for (int gl = 0; gl < 2; gl++) {
                unsigned room = gl ? UNIF_FX : (c || b) ? UNIF_CURVE : UNIF_FLAT;
                ok &= (pass_a[c][b][gl] = prog(ac[c][b][gl], an[c][b][gl], room)) != 0;
            }
    ok &= (pass_b[0] = prog(v3d_screen_code, V3D_SCREEN_CODE_N, UNIF_FLAT)) != 0;
    ok &= (pass_b[1] = prog(v3d_screen_curve_code, V3D_SCREEN_CURVE_CODE_N, UNIF_CURVE)) != 0;
    ok &= (pass_g = prog(v3d_glow_code, V3D_GLOW_CODE_N, UNIF_CURVE)) != 0;
    ok &= (pass_c = prog(v3d_afterglow_code, V3D_AFTERGLOW_CODE_N, UNIF_FLAT)) != 0;
    ok &= (hbuf[0] = heap(H_PITCH * H_ROWS)) != 0;
    ok &= (hbuf[1] = heap(H_PITCH * H_ROWS)) != 0;
    ok &= (gbuf = heap(GLOW_PITCH * GLOW_ROWS)) != 0;
    if (!ok) {
        aga_diag_log("[AGA] GPU: no room on the kernel's heap - the CRT look stays the display's own");
        return 0;
    }
    v3d_state |= V3D_MEMORY;
    aga_diag_log("[AGA] GPU: V3D 4.2 woken, programs ready");
    return 1;
}

int aga_v3d_ready(void) { return (v3d_state & V3D_MEMORY) != 0; }

/* ---- the settings (core 0, from aga_hvs.c's control words) ------------------ */
static volatile int set_curve, set_glow, set_after;
static const uint16_t after_decay[4] = { 0, 90, 141, 192 };

int aga_v3d_set(int what, int v)
{
    if (what == 0) set_curve = v < 0 ? 0 : v > 9 ? 9 : v;
    else if (what == 1) set_glow = v < 0 ? 0 : v > 100 ? 100 : v;
    else set_after = v < 0 ? 0 : v > 3 ? 3 : v;
    if (set_curve || set_glow || set_after) v3d_wake();
    return aga_v3d_wanted();
}

int aga_v3d_wanted(void)
{
    return (set_curve || set_glow || set_after) && aga_v3d_ready();
}

/* ---- the tables pass B reads (core 3) ------------------------------------
   rowB: an HDMI row's two lines of H and their weights, the row's scanline
   factor baked in, L | dl << 10 | wa << 12 | wb << 21. colt: a column's CRT
   mask factor. Both are 0..256 of a pixel kept, matching what the HVS does
   when it blends black over at the planes' alpha: (255 - alpha) / 255. All
   of it in the GPU's byte order, naturally. */
#define CRT_ROWS 1600
#define CRT_COLS 2560
static uint32_t crt_rowb[CRT_ROWS] __attribute__((aligned(64)));
static uint32_t crt_colt[CRT_COLS] __attribute__((aligned(64)));
static uint32_t tab_ow, tab_oh;

/* output pixel i's two source pixels across n of them at step (16.16): the
   first's index in *x0, the second's weight 0..256 - Smooth is bilinear at
   the pixel's centre, Sharp only where an edge between two crosses it */
static uint32_t crt_tap(uint32_t i, uint32_t step, uint32_t n, int look, uint32_t *x0)
{
    int64_t c = (int64_t)(2 * i + 1) * step / 2 - 32768;       /* (i + 0.5) x step - 0.5 */
    if (c < 0) c = 0;
    uint32_t x = (uint32_t)(c >> 16), f = (uint32_t)(c & 0xffff) >> 8;
    if (n < 2 || x >= n - 1) { *x0 = n ? n - 1 : 0; return 0; }
    *x0 = x;
    if (look == 1 || step >= 0x10000) return f;
    int32_t t = (((int32_t)f - 128) * 65536) / (int32_t)step + 128;
    return t < 0 ? 0 : t > 256 ? 256 : (uint32_t)t;
}

static inline uint32_t crt_factor(uint32_t alpha) { return ((255 - (alpha & 0xff)) * 256 + 127) / 255; }

int aga_v3d_tables(const uint8_t *row_dark, int flip, const uint8_t *col_dark, uint32_t ow, uint32_t oh,
                   uint32_t sh, uint32_t vstep, int look)
{
    if (ow > CRT_COLS || oh > CRT_ROWS || !sh) return 0;
    for (uint32_t y = 0; y < oh; y++) {
        uint32_t y0, f = crt_tap(y, vstep, sh, look, &y0);
        uint32_t rf = row_dark ? crt_factor(row_dark[flip ? oh - 1 - y : y]) : 256;
        uint32_t wa = ((256 - f) * rf + 128) >> 8, wb = (f * rf + 128) >> 8;
        crt_rowb[y] = __builtin_bswap32(y0 | (y0 + 1 < sh ? 1u : 0u) << 10 | wa << 12 | wb << 21);
    }
    for (uint32_t x = 0; x < ow; x++) crt_colt[x] = __builtin_bswap32(col_dark ? crt_factor(col_dark[x]) : 256);
    arm_flush_cache((uintptr_t)crt_rowb, oh * 4);
    arm_flush_cache((uintptr_t)crt_colt, ow * 4);
    tab_ow = ow;
    tab_oh = oh;
    return 1;
}

/* ---- a draw: its passes in turn ------------------------------------------------ */
static inline uint32_t f32_bits(float f) { union { float f; uint32_t u; } c = { .f = f }; return c.u; }
static inline uint32_t phys(const void *p) { return (uint32_t)mmu_virt2phys((uintptr_t)p); }
static inline uint32_t gpu_pitch(uint32_t w) { return (w + 15) / 16 * 64; }      /* 16-pixel blocks */

uint32_t aga_v3d_slot(int s, uint32_t *pitch, uint32_t ow)
{
    if (pitch) *pitch = gpu_pitch(ow);
    return (uint32_t)gpu_pa + (uint32_t)s * (gpu_bytes / 2);
}

struct pass { uint32_t code, unif, groups; };
static struct pass passes[4];
static int npass, cur;                  /* the draw's passes, the one running; -1 none */
static volatile int busy;
static uint32_t n0, t_started;
static uint64_t t0, t_pass;
static uint32_t us_pass[4], us_draw, us_max, need_us;
static int ag_cur;                      /* the H buffer this draw's pass A writes (Afterglow: in turn) */
static uint32_t ag_key;                 /* the last draw's crop height | H pitch << 12, 0 none */
static uint64_t ag_t;
static uint32_t drawn, late, skipped, blended;
static int draw_slot;

static void send(const struct pass *p)
{
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TFLSTA, 0);            /* the whole L2T, every bloody time */
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TFLEND, 0xffffffffu);
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TCACTL, 1);            /* L2T flush - the GPU stalls on it, tough shit */
    v3d_wr(V3D_CORE0 + V3D_CTL_SLCACTL, 0x0F0F0F0Fu);   /* slice caches: instructions, uniforms, TMU */
    n0 = (v3d_rd(V3D_CORE0 + V3D_CSD_STATUS) >> 4) & 0xff;
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(1), 1u << 16);       /* one work-group in Y */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(2), 1u << 16);       /* ... and Z */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(3), V3D_ROW_CFG3);
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(4), p->groups - 1);  /* batches - 1 */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(5), p->code);        /* one thread a QPU: up to 57 registers */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(6), p->unif);
    t_pass = v3d_now();
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(0), p->groups << 16);  /* the work-groups in X: it starts */
}

/* after a pass: shove what it wrote out of the TMU's write combiner and the
   L2T, for the next pass and the HVS - a few microseconds each; 0 too slow */
static int clean(uint64_t limit)
{
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TCACTL, 1u << 8);
    uint64_t t = v3d_now();
    while (v3d_rd(V3D_CORE0 + V3D_CTL_L2TCACTL) & (1u << 8)) if (v3d_now() - t > limit) return 0;
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TCACTL, 2u << 1 | 1);
    while (v3d_rd(V3D_CORE0 + V3D_CTL_L2TCACTL) & 1) if (v3d_now() - t > limit) return 0;
    return 1;
}

static void put_unif(uint8_t *room, unsigned at, const uint32_t *u, int n)
{
    uint32_t *d = (uint32_t *)(room + at);
    for (int i = 0; i < n; i++) d[i] = __builtin_bswap32(u[i]);
    arm_flush_cache((uintptr_t)d, (uint32_t)n * 4);
}

int aga_v3d_start(const struct aga_v3d_frame *f, int slot)
{
    if (!aga_v3d_wanted()) return 0;
    if (busy) { skipped++; return 0; }
    uint32_t ow = f->ow, oh = f->oh;
    uint32_t nblk = (ow + 15) / 16, opitch = nblk * 64;
    if (ow != tab_ow || oh != tab_oh || ow < 16 || oh < 8 || f->sh > H_ROWS || f->sh < 2 || opitch > H_PITCH ||
        (uint64_t)opitch * oh > gpu_bytes / 2)
        return 0;
    int curve = set_curve, glow = set_glow > 0, after = set_after;
    int blend = f->prev != 0;
    uint64_t frq = v3d_freq(), now = v3d_now();
    uint32_t out = aga_v3d_slot(slot, 0, ow);
    /* Afterglow: two H buffers in turn, the frame before's when that was
       the last one drawn, same crop height and pitch, within 60 ms - an
       old picture must never come back as a ghost */
    uint32_t ag_k = (f->sh & 0xfff) | opitch << 12;
    int ag_prev = after && ag_key == ag_k && now - ag_t < frq * 60 / 1000;
    uint8_t *h = hbuf[after ? ag_cur : 0];
    uint32_t hb = phys(h);
    uint32_t k = (f->look == 1 || f->stepx >= 0x10000 || !f->stepx) ? 256 : (uint32_t)((65536ull << 8) / f->stepx);
    uint32_t swm1 = f->sw ? f->sw - 1 : 0, shm1 = f->sh - 1;
    npass = 0;
    if (glow) {                         /* pass G: a work-group a row of the glow buffer */
        uint32_t gw = (f->sw + 7) / 8, gh = (f->sh + 3) / 4;
        if (gw > GLOW_COLS) gw = GLOW_COLS;
        if (gh > GLOW_ROWS) gh = GLOW_ROWS;
        const uint32_t ug[8] = { gh - 1, f->src, f->pitch, phys(gbuf), GLOW_PITCH, (gw + 15) / 16, swm1, shm1 };
        put_unif(pass_g, UNIF_CURVE, ug, 8);
        passes[npass++] = (struct pass){ phys(pass_g), phys(pass_g + UNIF_CURVE), gh };
    }
    {   /* pass A: shm1, src, spitch, hbuf, hpitch, nblk, then the flat pass's
           step, start, k, swm1 - or a curved one's k, swm1 and its floats -
           then Frame blend's dsrc, Glow's seven */
        uint32_t a[28];
        int n = 0;
        if (curve) {
            float kx = 0.02f * (float)curve, sw = (float)f->sw;
            const uint32_t c[14] = { shm1, f->src, f->pitch, hb, opitch, nblk, k, swm1,
                                     f32_bits(1.0f / (float)ow), f32_bits(sw * 65536.0f),
                                     f32_bits((0.5f * sw - 0.5f) * 65536.0f), f32_bits(1.0f / (float)f->sh),
                                     f32_bits(kx), f32_bits(1.0f / (1.0f + 0.25f * kx)) };
            for (int i = 0; i < 14; i++) a[n++] = c[i];
        } else {
            const uint32_t c[10] = { shm1, f->src, f->pitch, hb, opitch, nblk, f->stepx, (f->stepx >> 1) - 32768u, k,
                                     swm1 };
            for (int i = 0; i < 10; i++) a[n++] = c[i];
        }
        if (blend) a[n++] = f->prev - f->src;
        if (glow) {
            uint32_t gw = (f->sw + 7) / 8, gh = (f->sh + 3) / 4;
            if (gw > GLOW_COLS) gw = GLOW_COLS;
            if (gh > GLOW_ROWS) gh = GLOW_ROWS;
            const uint32_t c[7] = { phys(gbuf), GLOW_PITCH, gh - 1, (uint32_t)set_glow * 256 / 100, gw - 1,
                                    (uint32_t)-229376, 0x01000100u };
            for (int i = 0; i < 7; i++) a[n++] = c[i];
        }
        uint8_t *am = pass_a[curve ? 1 : 0][blend][glow];
        unsigned at = glow ? UNIF_FX : (curve || blend) ? UNIF_CURVE : UNIF_FLAT;
        put_unif(am, at, a, n);
        passes[npass++] = (struct pass){ phys(am), phys(am + at), f->sh };
    }
    if (ag_prev) {                      /* pass C: shm1, hcur, hprev, hpitch, nblk, decay */
        const uint32_t uc[6] = { shm1, hb, phys(hbuf[ag_cur ^ 1]), opitch, nblk, after_decay[after] };
        put_unif(pass_c, UNIF_FLAT, uc, 6);
        passes[npass++] = (struct pass){ phys(pass_c), phys(pass_c + UNIF_FLAT), f->sh };
    }
    if (curve) {
        /* pass B curved: ohm1, hbuf, hpitch, out, opitch, nblk, colt, shm1, then
           1/ow, 1/oh, ky, bsy, sh, sh / 2 - 1/2, kv (Sharp: the rows a line),
           the scanlines' strength 0..1 times 16 */
        float ky = 0.03f * (float)curve, sh = (float)f->sh;
        float kv = f->look == 1 || oh <= f->sh ? 1.0f : (float)oh / sh;
        const uint32_t ub[16] = { oh - 1, hb, opitch, out, opitch, nblk, phys(crt_colt), shm1,
                                  f32_bits(1.0f / (float)ow), f32_bits(1.0f / (float)oh), f32_bits(ky),
                                  f32_bits(1.0f / (1.0f + 0.25f * ky)), f32_bits(sh), f32_bits(0.5f * sh - 0.5f),
                                  f32_bits(kv), f32_bits((float)(f->scan_pct > 100 ? 100 : f->scan_pct) * 0.16f) };
        put_unif(pass_b[1], UNIF_CURVE, ub, 16);
        passes[npass++] = (struct pass){ phys(pass_b[1]), phys(pass_b[1] + UNIF_CURVE), oh };
    } else {                            /* pass B: ohm1, hbuf, hpitch, out, opitch, nblk, rowB, colt */
        const uint32_t ub[8] = { oh - 1, hb, opitch, out, opitch, nblk, phys(crt_rowb), phys(crt_colt) };
        put_unif(pass_b[0], UNIF_FLAT, ub, 8);
        passes[npass++] = (struct pass){ phys(pass_b[0]), phys(pass_b[0] + UNIF_FLAT), oh };
    }
    if (after) {                        /* this frame's H becomes the one before, for the next */
        ag_key = ag_k;
        ag_t = now;
        ag_cur ^= 1;
    } else {
        ag_key = 0;
    }
    if (blend) blended++;
    __asm__ volatile("dsb sy" ::: "memory");
    draw_slot = slot;
    busy = 1;
    cur = 0;
    t0 = now;
    send(&passes[0]);
    return 1;
}

/* core 3: keep the draw moving. 0 nothing to do or still at it, 1 the
   picture is whole in its slot (*slot), -1 the fucker failed: the GPU is
   not used again until a setting changes. A pass gets 20 ms. */
int aga_v3d_poll(int *slot)
{
    if (!busy) return 0;
    uint64_t frq = v3d_freq(), now = v3d_now();
    if (((v3d_rd(V3D_CORE0 + V3D_CSD_STATUS) >> 4) & 0xff) == n0) {
        if (now - t_pass < frq / 50) return 0;
        late++;
        busy = 0;
        v3d_state &= ~V3D_MEMORY;       /* the GPU may be at it still: off for good this boot */
        v3d_state |= V3D_TRIED;
        aga_diag_log("[AGA] GPU: a pass took more than 20 ms - the CRT look is the display's own now");
        return -1;
    }
    us_pass[cur] = (uint32_t)((now - t_pass) * 1000000 / frq);
    if (!clean(frq / 1000)) {
        busy = 0;
        return -1;
    }
    if (++cur < npass) {
        send(&passes[cur]);
        return 0;
    }
    busy = 0;
    us_draw = (uint32_t)((v3d_now() - t0) * 1000000 / frq);
    /* a draw over 30 ms was only seen late: the frames stopped for a while
       (a game between its screens) and the polls with them - 80 ms in
       Agony's loading on the CM4. Not the GPU's time: I leave it out. */
    if (us_draw < 30000) {
        if (us_draw > us_max) us_max = us_draw;
        need_us = us_draw >= need_us ? us_draw : need_us - (need_us - us_draw) / 64;   /* up at once, a 64th down */
    }
    if (++drawn % 1000 == 1) {
        static char line[120];
        char *p = line;
        const char *t = "[AGA] GPU: drawn ";
        while (*t) *p++ = *t++;
        uint32_t v[4] = { drawn, us_draw, us_max, skipped };
        const char *sep[4] = { ", last ", " us, worst ", " us, frames not drawn ", "" };
        for (int i = 0; i < 4; i++) {
            char d[12];
            int nd = 0;
            uint32_t x = v[i];
            do { d[nd++] = (char)('0' + x % 10); x /= 10; } while (x);
            while (nd) *p++ = d[--nd];
            for (t = sep[i]; *t; t++) *p++ = *t;
        }
        *p = 0;
        aga_diag_log(line);
    }
    *slot = draw_slot;
    return 1;
}

int aga_v3d_busy(void) { return busy; }
uint32_t aga_v3d_need_us(void) { return need_us; }
