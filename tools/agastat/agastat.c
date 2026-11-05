/* agastat - read the AGA-PISTORM diagnostics out of the Pi.
 *
 *   agastat           counters and the log ring
 *   agastat LOG       just the log
 *   agastat WATCH     counters every second until Ctrl-C
 *   agastat TRAPS     the last custom-register accesses the 68k made, oldest
 *                     first. AGA_DIAG kernels only; empty on a shipping build.
 *   agastat REGS      the virtual chipset's display state: does the thing on
 *                     screen have bitplanes, a window and pointers, or not?
 *                     Counters say HOW MANY registers were touched; when a game
 *                     stops dead this says WHICH, which is the only question
 *                     left at that point.
 *
 * Works with no picture and no serial cable, which is the point: if the first
 * boot on real hardware shows nothing, caffed still dials the PC and this can
 * be run remotely to find out how far the Pi got.
 *
 * Interface: write a selector to $DFF1E8, then read words from $DFF1EA.
 * Build: m68k-amigaos-gcc -O2 -noixemul -o agastat agastat.c
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#define DIAG_SEL  (*(volatile UWORD *)0xDFF1E8)
#define DIAG_DAT  (*(volatile UWORD *)0xDFF1EA)
#define SEL_LOG   0x100
#define SEL_MAGIC 0x200
#define SEL_TRAPS 0x300
#define SEL_REGS  0x400
#define SEL_DEEP  0x500
#define SEL_PCS   0x600
#define SEL_HIST  0x700
#define SEL_SER   0x800
#define SEL_CODE  0x900
#define SEL_CODE2 0x1200
#define SEL_TRACE 0x1300
#define SEL_PREV_Q 0x1600
#define SEL_PREV_ON 0x1601
#define SEL_PREV_OFF 0x1602
#define SEL_PCHIP 0x1800
#define SEL_PREGS 0x1801
#define SEL_PPROF 0x1802
#define PPROF_WORDS (2 + 4096 * 4 + 12 * (2 + 256) + 4096)
#define SEL_PROF  0xA00
#define SEL_PCE   0xB00
#define SEL_BEAM  0x1100
#define SEL_WAVE  0xC00
#define SEL_TRIG  0xD00
#define SEL_ENV   0xE00
#define SEL_WRAP  0xF00
#define SEL_AUDW  0x1000
#define AUDWOK  "agastat: %ld sound-register writes saved to %s\n"
#define AUDWERR "agastat: cannot open the output file\n"

/* Kept as named constants because a heredoc eats the backslashes. */
#define NOSAMP  "--- profile: no samples (no interrupts, or an older kernel) ---\n"
#define PROFHDR "--- where the 68k was, %ld samples at interrupt dispatch ---\n"
#define PROFROW "  $%08lx  %8ld  %3ld%%\n"
#define WRAPNONE "--- no loop-backs recorded (or an older kernel) ---\n"
#define WRAPHDR  "--- what each channel loops back to (every 512th loop) ---\n  ch      AUDxLC   len   peak of the loop\n"
#define WRAPROW  "  %2ld  $%08lx  %4ld       %4ld\n"
#define ENVNONE "--- no per-channel loudness recorded (or an older kernel) ---\n"
#define ENVHDR  "--- per-channel loudness, one row per frame (ch0 ch1 ch2 ch3) ---\n"
#define ENVROW  "  %4ld   %3ld %3ld %3ld %3ld\n"
#define TRIGNONE "--- no channel starts recorded (or an older kernel) ---\n"
#define TRIGHDR  "--- the last audio channel starts, oldest first ---\n  ch      AUDxLC   len   per  vol   first word\n"
#define TRIGROW  "  %2ld  $%08lx  %4ld  %4ld  %3ld       $%04lx\n"
#define WAVEERR "agastat: cannot open the output file\n"
#define WAVEOK  "agastat: wrote %ld stereo frames to %s\n"
#define PCEHDR  "--- the last 32 exact interrupt PCs, oldest first ---\n"
#define PCEROW  "  $%08lx\n"
#define BEAMHDR "--- the last beam bytes a raster wait was handed ---\n  what value   gap us   step\n"
#define BEAMROW "  %s %4ld  %7ld  %5ld\n"
#define COVROW  "  low bytes handed out: %ld of %ld (first missing: %ld)\n"
#define COVLIST "  never handed out:\n"
#define COVWHAT "--- %s ---\n"
#define COVGAP  "    %3ld..%3ld\n"
#define COVBITS "  bits ever set: $%04lx, bits always set: $%04lx\n"

/* Index-matched with the enum in arm/aga_glue.h, which is append-only: an older
   tool against a newer kernel simply stops naming the last few. */
static const char *const names[] = {
    "frames", "frame_us_last", "frame_us_worst", "loop_lag_max_cck",
    "loop_resyncs", "audio_streams", "audio_resyncs", "video_presents",
    "video_errors", "sandbox_enters", "sandbox_leaves", "disk_commands",
    "reboots_requested", "traps_read", "traps_write", "state", "bpl_frames",
    "ipl_hook", "irq_from_virtual",
    "irq_l1", "irq_l2", "irq_l3", "irq_l4", "irq_l5", "irq_l6", "irq_l7",
    "real_intena", "real_intreq", "real_dmacon",
    "blits", "bpl_data_frames",
    "chip_reads", "chip_writes",
    "chipw_0-256k", "chipw_256-512k", "chipw_512-768k", "chipw_768k-1m",
    "chipw_1m-1.25m", "chipw_1.25-1.5m", "chipw_1.5-1.75m", "chipw_1.75-2m",
    "aud0_on", "aud1_on", "aud2_on", "aud3_on",
    "aud0_words", "aud1_words", "aud2_words", "aud3_words",
    "aud_nudges", "aud_drops", "aud_repeats",
    "aud0_energy", "aud1_energy", "aud2_energy", "aud3_energy",
    "aud0_vol", "aud1_vol", "aud2_vol", "aud3_vol",
    "aud0_volmax", "aud1_volmax", "aud2_volmax", "aud3_volmax",
    "aud0_audible", "aud1_audible", "aud2_audible", "aud3_audible",
    "aud0_volwr", "aud1_volwr", "aud2_volwr", "aud3_volwr",
    "aud0_volnz", "aud1_volnz", "aud2_volnz", "aud3_volnz",
    "aud0_dmaoff", "aud1_dmaoff", "aud2_dmaoff", "aud3_dmaoff",
    "aud0_wrap", "aud1_wrap", "aud2_wrap", "aud3_wrap",
    "aud_intena",
    "dlist_words_inuse",
    "dlist_free_at",
    "dlist_free_len",
    "ptr_in_list", "ptr_in_planes", "ptr_in_ctl",
    "ptr_in_pos0", "ptr_in_image", "ptr_in_palette",
    "ptr_out_list", "ptr_out_planes", "ptr_out_ctl",
    "ptr_out_pos0", "ptr_out_image", "ptr_out_palette",
    "ptr_now_list", "ptr_now_planes", "ptr_now_ctl",
    "ptr_now_pos0", "ptr_now_image", "ptr_now_palette",
    "loop_busy_us", "loop_busy_worst", "loop_wait_us",   /* the frame's budget on core 3 */
    "audio_us_frame", "audio_us_worst",                    /* time on the PiStorm bus for audio */
    "lock_us_frame", "lock_us_worst",                      /* core 3 waiting for the chipset lock */
    "soc_temp_mc", "throttled_flags",                      /* firmware, sampled at sandbox leave */
    "bus_us_frame", "bus_us_worst",                        /* three real-register reads per frame */
    "cyc_us_frame", "cyc_us_worst", "stat_us_worst", "body_us_worst", "core3_daif",
    "cyc_frame_worst", "chipw_frame", "chipw_frame_worst", "mem_us_last", "mem_us_worst",
    "chip_real_kb",
    "abort_us_frame", "abort_us_worst", "aborts_frame", "aborts_worst",   /* core 0: the chip RAM redirect's cost */
    "l1_refill_frame", "l1_refill_worst", "l2_refill_frame", "l2_refill_worst",   /* core 3 PMU */
    "watch_samples", "watch_list_changes", "watch_stat_moves",                     /* the HVS after the hand-back */
    "watch_lact_first", "watch_list_last", "watch_lact_last",
    "watch_stat_last", "watch_ctl_last", "watch_ptr0_last", "watch_latch_us",
    "frames_skipped",                                                             /* Pi 3 frame skip */
    "build_board", "run_board",                                                   /* 3 or 4: kernel built for, Pi found */
    "lag_0-2ms", "lag_2-5ms", "lag_5-10ms", "lag_10-20ms", "lag_20-40ms", "lag_40ms+",   /* frames by loop lag */
    "beam_pairs", "beam_pair_skips", "beam_pair_us_worst",                          /* two-byte line reads */
    "loop_vpos_max", "trap_vpos_max", "trap_add_max", "core_lines", "core_maxhpos", /* the beam from both sides */
    "geom_changes", "geom_from", "geom_to", "geom_min", "geom_max",                 /* PAL 313 / NTSC 263 */
    "beamcon0_writes", "beamcon0_last", "beamcon0_ntsc_writes", "beamcon0_ntsc_pc",
    "geom_frame", "geom_beamcon0", "geom_in_sandbox",
    "beamcon0_ntsc_in_sandbox", "beamcon0_ntsc_frame", "beamcon0_copper",
    "dff005_bits_seen", "dff005_reads", "dff005_bit8_set",
    "dmacon_now", "bplen_changes", "bplen_off_frame",                            /* the display going out */
    "bplen_on_frame", "dmacon_writes", "dmacon_bplen_off_writes",
    "dmacon_off_pc", "dmacon_off_value", "dmacon_last_value",
    "bpl1pt_now", "bpl1pt_changes", "bpl1_nonzero_of_256",                        /* what the display reads */
    "bpl1pt_min", "bpl1pt_max",
    "diwstrt_now", "diwstop_now", "ddfstrt_now", "ddfstop_now", "bplcon0_now",
    "dmalog0", "dmalog1", "dmalog2", "dmalog3",                                   /* value | copper<<16 | frame<<17 */
    "dmalog4", "dmalog5", "dmalog6", "dmalog7",
    "fetch_decisions", "fetch_no_window", "fetch_no_dma",                          /* per frame */
    "fetch_no_planes", "fetch_too_many_planes", "fetch_started",
    "chunk_seq", "chunk_vpos", "chunk_hpos",                                      /* chipset heartbeat */
    "last_trap_reg", "last_trap_value", "last_trap_rw",
    "bc0log0", "bc0log1", "bc0log2", "bc0log3",                                   /* value | copper<<16 | frame<<17 */
    "bc0log4", "bc0log5", "bc0log6", "bc0log7",
    "cpu_stalled", "cpu_stall_frame",
    "stall_68k_sr", "stall_emu68_intf", "stall_intena",
    "stall_intreq", "stall_ipl_wanted",
    "cia_wakeups", "stall_insn_count", "stall_irq_check",
    "arm_irqs_held_from_game",
    "intreqr_poll_pc", "intreqr_last", "intreqr_bits_ever", "intreqr_bits_always",
    "intreqr_reads",
    "mouse_quits",
    "exter_from_real_cia", "exter_from_emu68_arm",
    "audirq_dat_on_idle", "audirq_wrap", "audirq_first_word",
    "audirq_manual_word", "audirq_playout",
    "real_intena_fixes", "real_intena_stray_bits",
    "bplpt_cpu_writes", "bplpt_writes_in_display", "bplpt_line_last", "bplpt_late_line_max",
    "bplpt_lines_0-39", "bplpt_lines_40-79", "bplpt_lines_80-119", "bplpt_lines_120-159",
    "bplpt_lines_160-199", "bplpt_lines_200-239", "bplpt_lines_240-279", "bplpt_lines_280+",
    "blits_finished", "blit_irqs_swallowed", "blit_swallowed_frame", "blit_irqs_reraised",
    "trapbus_us_frame", "trapbus_us_worst", "trapbus_writes_frame",
    "traphold_us_frame", "traphold_us_worst",
    "fwd_intena", "fwd_intena_nop", "fwd_intreq", "fwd_adkcon",
    "fwd_adkcon_nop", "fwd_potgo", "fwd_potgo_nop", "fwd_joytest",
    "fwd_serial", "fwd_disk", "fwd_other",
    "busw_us_68k", "busw_us_core1", "busw_us_hkeep", "busw_us_chipset",
    "bush_us_68k", "bush_us_core1", "bush_us_hkeep", "bush_us_chipset",
    "busn_68k", "busn_core1", "busn_hkeep", "busn_chipset",
    "busw_worst_us_68k", "busw_worst_us_hkeep", "busw_worst_us_chipset",
    "abort_us_total", "aborts_total",
    "soc_calls", "soc_code", "soc_temp_enter_mc", "soc_throttled_enter",
    "exter_storms", "exter_storm_icr",
    "mbox_foreign", "mbox_timeouts",
    "trap_writes_128bit", "trap_reads_128bit", "trap_odd_size",
    "bplcon0_cpu_writes",
    "bc0w_val0", "bc0w_val1", "bc0w_val2", "bc0w_val3",
    "bc0w_val4", "bc0w_val5", "bc0w_val6", "bc0w_val7",
    "bc0w_pc0", "bc0w_pc1", "bc0w_pc2", "bc0w_pc3",
    "bc0w_pc4", "bc0w_pc5", "bc0w_pc6", "bc0w_pc7",
    "coprs_a0", "coprs_a1", "coprs_a2", "coprs_a3",
    "coprs_a4", "coprs_a5", "coprs_a6", "coprs_a7",
    "coprs_f0", "coprs_f1", "coprs_f2", "coprs_f3",
    "coprs_f4", "coprs_f5", "coprs_f6", "coprs_f7",
    "coprs_changes", "cop_bc0_writes", "cop_bc0_pc", "cop_bc0_val", "cop_bc0_frame",
    "copw_n",
    "copw_rv0", "copw_rv1", "copw_rv2", "copw_rv3",
    "copw_rv4", "copw_rv5", "copw_rv6", "copw_rv7",
    "copw_pc0", "copw_pc1", "copw_pc2", "copw_pc3",
    "copw_pc4", "copw_pc5", "copw_pc6", "copw_pc7",
    "bc0w_last_frame",
    "evlog_n", "ev00_a", "ev00_b", "ev01_a", "ev01_b", "ev02_a",
    "ev02_b", "ev03_a", "ev03_b", "ev04_a", "ev04_b", "ev05_a",
    "ev05_b", "ev06_a", "ev06_b", "ev07_a", "ev07_b", "ev08_a",
    "ev08_b", "ev09_a", "ev09_b", "ev10_a", "ev10_b", "ev11_a",
    "ev11_b", "ev12_a", "ev12_b", "ev13_a", "ev13_b", "ev14_a",
    "ev14_b", "ev15_a", "ev15_b", "ev16_a", "ev16_b", "ev17_a",
    "ev17_b", "ev18_a", "ev18_b", "ev19_a", "ev19_b", "ev20_a",
    "ev20_b", "ev21_a", "ev21_b", "ev22_a", "ev22_b", "ev23_a",
    "ev23_b", "ev24_a", "ev24_b", "ev25_a", "ev25_b", "ev26_a",
    "ev26_b", "ev27_a", "ev27_b", "ev28_a", "ev28_b", "ev29_a",
    "ev29_b", "ev30_a", "ev30_b", "ev31_a", "ev31_b", "ev32_a",
    "ev32_b", "ev33_a", "ev33_b", "ev34_a", "ev34_b", "ev35_a",
    "ev35_b", "ev36_a", "ev36_b", "ev37_a", "ev37_b", "ev38_a",
    "ev38_b", "ev39_a", "ev39_b", "ev40_a", "ev40_b", "ev41_a",
    "ev41_b", "ev42_a", "ev42_b", "ev43_a", "ev43_b", "ev44_a",
    "ev44_b", "ev45_a", "ev45_b", "ev46_a", "ev46_b", "ev47_a",
    "ev47_b",
    "jit_new", "jit_new_chip", "code_direct",   /* translations in the sandbox; the soft-cache bypass on/off */
    "bustest_mode", "bustest_bytes", "bustest_us", "bustest_worst_ns",   /* agaboot BUSTEST */
    /* HAM6 on the RGB port (core 1): state 0 off 1 on 2 no core 3 refused 4 Position
       5 black: the OS's own display held back in a WHDLoad game */
    "rgb_state", "rgb_frames", "rgb_fps10", "rgb_words_last", "rgb_kb_total",
    "rgb_snap_us", "rgb_encode_us", "rgb_write_us", "rgb_wait_us",
    "rgb_torn", "rgb_palettes", "rgb_skipped",
    "rgb_offset_xy", "rgb_size_wh", "rgb_kmeans", "rgb_hdmi_off",   /* x<<16|y (signed 16 each), w<<16|lines */
    /* HDMI audio: state 0 off 1 on 2 no link (or no picture) 3 no clock 4 slot taken 5 no DMA
       6 unused since 1.112 7 switched off 8 another driver's MAI */
    "hdmi_state", "hdmi_dma_ch", "hdmi_lead", "hdmi_ppm",
    "hdmi_resyncs", "hdmi_idles", "hdmi_fixups",
    "hdmi_hsm_khz", "hdmi_hsm_set_khz",   /* measured / as the ARM set it (Pi 3: CM_HSMCTL) */
    "hdmi_mai_errs",                      /* looks that found the MAI's error flags set */
    "hdmi_scan_stat", "hdmi_scan_moves",  /* HVS channel 1's status; changes seen (the HDMI picture moving) */
    "hdmi_filter",                        /* 0 off 1 A500 2 A1200, +16 while the power LED is lit */
    "audio_mix",                          /* stereo width % (both outputs), +256 left and right swapped */
    "crop",                               /* 1 custom crop, +2 crop screen on a game picture, +4 NTSC, +8/+16 a last picture (NTSC) */
    "auto_x",                             /* Size Auto's window: x << 16 | width */
    "auto_y",                             /* ...y << 16 | height, +$8000 NTSC */
    "hdmi_sync",                          /* 1 on, +2 PV good / +4 refused, reach+1 << 4, porch << 8, phase << 16 */
    "hdmi_sync_need",                     /* porch lines a frame needs, 16.16 */
    "hdmi_sync_slips",                    /* frames repeated or dropped since sync began */
    "hdmi_sync_pv",                       /* the mode's porch << 16 | active height */
    "ahi",                                /* 1 understood +2 ring on +4 HDMI running, frames waiting/64 << 8, empty << 16 */
};
#define NAMED (sizeof names / sizeof names[0])

static ULONG read32(int sel)
{
    /* counters from 256 up: selector $100 is the log, so they live at $1700 */
    if (sel >= 0x100 && sel < 0x200) sel = 0x1700 + (sel - 0x100);
    Disable();
    DIAG_SEL = (UWORD)sel;
    ULONG hi = DIAG_DAT;
    ULONG lo = DIAG_DAT;
    Enable();
    return (hi << 16) | lo;
}

/* Named because a bare $100 tells you nothing at three in the morning. Only the
   registers that matter while a game is setting up or has just stopped; the
   rest print as their offset. */
static const char *regname(int r)
{
    switch (r & 0x1FE) {
    case 0x000: return "BLTDDAT";  case 0x002: return "DMACONR";
    case 0x004: return "VPOSR";    case 0x006: return "VHPOSR";
    case 0x00A: return "JOY0DAT";  case 0x00C: return "JOY1DAT";
    case 0x010: return "ADKCONR";  case 0x016: return "POTGOR";
    case 0x01C: return "INTENAR";  case 0x01E: return "INTREQR";
    case 0x034: return "POTGO";    case 0x07C: return "DENISEID";
    case 0x080: return "COP1LCH";  case 0x082: return "COP1LCL";
    case 0x084: return "COP2LCH";  case 0x086: return "COP2LCL";
    case 0x088: return "COPJMP1";  case 0x08A: return "COPJMP2";
    case 0x08E: return "DIWSTRT";  case 0x090: return "DIWSTOP";
    case 0x092: return "DDFSTRT";  case 0x094: return "DDFSTOP";
    case 0x096: return "DMACON";   case 0x09A: return "INTENA";
    case 0x09C: return "INTREQ";   case 0x09E: return "ADKCON";
    case 0x100: return "BPLCON0";  case 0x102: return "BPLCON1";
    case 0x104: return "BPLCON2";  case 0x106: return "BPLCON3";
    case 0x108: return "BPL1MOD";  case 0x10A: return "BPL2MOD";
    case 0x10C: return "BPLCON4";  case 0x1E4: return "DIWHIGH";
    case 0x1FC: return "FMODE";
    default: return 0;
    }
}

static void dump_traps(void)
{
    Disable();
    DIAG_SEL = SEL_TRAPS;
    UWORD n = DIAG_DAT;
    Enable();
    if (!n) {
        PutStr("--- traps --- ring is empty.\n"
               "  Either this is not an AGA_DIAG kernel, or nothing has been trapped yet.\n");
        return;
    }
    Printf("--- last %ld custom-register accesses, oldest first ---\n", (LONG)n);
    for (int i = 0; i < n; i++) {
        Disable();
        UWORD hi = DIAG_DAT, lo = DIAG_DAT, pch = DIAG_DAT, pcl = DIAG_DAT;
        Enable();
        int wr = (hi >> 15) & 1, reg = hi & 0x1FF;
        ULONG pc = ((ULONG)pch << 16) | pcl;
        const char *nm = regname(reg);
        char rbuf[8];
        if (!nm) { sprintf(rbuf, "$%03x", reg); nm = rbuf; }
        Printf("  %s %-8s $%04lx  @%08lx\n", (ULONG)(wr ? "W" : "R"), (ULONG)nm, (ULONG)lo, pc);
    }
}

/* Names must match regs_of_interest[] in arm/aga_diag.c, in order. */
static const char *const regs_names[] = {
    "BPLCON0", "BPLCON1", "BPLCON2", "BPLCON3", "BPLCON4", "FMODE",
    "DIWSTRT", "DIWSTOP", "DIWHIGH", "DDFSTRT", "DDFSTOP",
    "BPL1MOD", "BPL2MOD",
    "BEAMCON0", "HTOTAL", "VTOTAL", "VBSTRT",
    "COP1LCH", "COP1LCL", "COP2LCH", "COP2LCL",
};
#define REGS_NAMED (sizeof regs_names / sizeof regs_names[0])

/* The deepest display seen since power-on: the frame with the most bitplanes.
   The high-water snapshot is the LAST one, and a quitting WHDLoad game leaves
   the OS's one-plane view there, on top of the in-game setup I fucking wanted. */
static void dump_deep(void)
{
    Disable(); DIAG_SEL = SEL_DEEP; UWORD planes = DIAG_DAT; UWORD n = DIAG_DAT; Enable();
    if (!planes || n > 64) { PutStr("\n--- deepest display: none seen (or older kernel) ---\n"); return; }
    UWORD v[64], st[3]; ULONG bpl[8];
    for (int i = 0; i < n; i++) { Disable(); v[i] = DIAG_DAT; Enable(); }
    for (int i = 0; i < 3; i++) { Disable(); st[i] = DIAG_DAT; Enable(); }
    for (int i = 0; i < 8; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); bpl[i] = ((ULONG)h << 16) | l; }
    Printf("\n--- deepest display seen: %ld bitplanes ---\n", (ULONG)planes);
    for (int i = 0; i < n; i++)
        Printf("  %-9s $%04lx\n", (ULONG)(i < (int)REGS_NAMED ? regs_names[i] : "?"), (ULONG)v[i]);
    Printf("  %-9s $%04lx\n  %-9s $%04lx\n  %-9s $%04lx\n", (ULONG)"DMACON", (ULONG)st[0], (ULONG)"INTENA", (ULONG)st[1], (ULONG)"INTREQ", (ULONG)st[2]);
    for (int i = 0; i < 8; i++) Printf("  BPL%ldPT    $%08lx\n", (ULONG)(i + 1), bpl[i]);
}

/* Where the 68k was, one sample per presented frame. Frozen when the game's
   display went away, so these are the game's last two dozen frames. */
/* Which registers the game actually touched, busiest first, and the values the
   beam reads handed back. Frozen and zeroed with everything else, so this
   describes the blank-screen period and nothing before it. A raster wait for a
   position I never fucking produce shows up as an empty value bucket. */
/* Everything the 68k transmitted on the serial line since the sandbox was
   entered. WHDLoad's exception handler dumps its register state here and then
   waits for a keypress from a terminal that is not attached - so when a game
   appears to hang, this is where it says why. */
static void dump_serial(void)
{
    Disable(); DIAG_SEL = SEL_SER; UWORD n = DIAG_DAT; Enable();
    if (!n) { PutStr("--- serial: nothing transmitted ---\n"); return; }
    Printf("--- %ld characters sent on the serial line ---\n", (ULONG)n);
    char line[81];
    int col = 0;
    for (int i = 0; i < n; i++) {
        Disable(); UWORD c = DIAG_DAT; Enable();
        char ch = (char)(c & 0xFF);
        if (ch == 10 || ch == 13 || col >= 78) {
            line[col] = 0; PutStr(line); PutStr("\n"); col = 0;
            if (ch == 10 || ch == 13) continue;
        }
        line[col++] = (ch >= 32 && ch < 127) ? ch : '.';
    }
    if (col) { line[col] = 0; PutStr(line); PutStr("\n"); }
}

/* The 68k code at the PC I froze at, as words, ready to disassemble.

   Every other section says WHAT registers a stuck game touches; none says WHO
   is touching them. This does. Printed as plain hex so the host can feed it
   straight to a 68k disassembler. */
static void dump_code(void)
{
    Disable(); DIAG_SEL = SEL_CODE; UWORD hi = DIAG_DAT; Enable();
    Disable(); UWORD lo = DIAG_DAT; Enable();
    ULONG base = ((ULONG)hi << 16) | lo;
    if (!base) { PutStr("--- no code captured: the machine never froze ---\n"); return; }
    Printf("--- 68k code at $%08lx (the frozen PC is $%08lx) ---\n",
           base, (ULONG)(base + 64));
    for (int row = 0; row < 16; row++) {
        Printf("$%08lx ", (ULONG)(base + row * 16));
        for (int i = 0; i < 8; i++) {
            Disable(); UWORD w = DIAG_DAT; Enable();
            Printf("%04lx ", (ULONG)w);
        }
        PutStr("\n");
    }
}

/* Where the 68k really spends its time: the PC sampled at every interrupt
   dispatch, bucketed to 256 bytes, busiest first. */
/* Write the exact bytes the DAC was given to a file, raw interleaved stereo
   8-bit unsigned-offset-binary at 27928 Hz. Fetched to the PC and turned into a
   WAV, it settles by ear what no counter has managed to: whether the sound
   effects are in my fucking output at all. */
static void dump_wave(const char *path)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    if (!f) { PutStr(WAVEERR); return; }
    Disable(); DIAG_SEL = SEL_WAVE; UWORD nh = DIAG_DAT; UWORD nl = DIAG_DAT; Enable();
    ULONG n = ((ULONG)nh << 16) | nl;
    if (n > 105000) n = 105000;
    static UBYTE buf[512];
    ULONG done = 0;
    while (done < n) {
        int k = 0;
        while (k < 512 && done < n) {
            Disable(); UWORD w = DIAG_DAT; Enable();
            buf[k++] = (UBYTE)(w >> 8);     /* left  */
            buf[k++] = (UBYTE)(w & 0xFF);   /* right */
            done++;
        }
        Write(f, buf, k);
    }
    Close(f);
    Printf(WAVEOK, (ULONG)n, (ULONG)path);
}

/* Every write the game made to the sound registers, in order, raw: 12 bytes an
   entry, big-endian - size<<28|frame, vpos<<20|hpos<<10|reg, value. Decoded on
   the PC, where lining it up against the audio is easy. */
static void dump_audw(const char *path)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    if (!f) { PutStr(AUDWERR); return; }
    Disable(); DIAG_SEL = SEL_AUDW; UWORD n = DIAG_DAT; Enable();
    if (n > 4096) n = 4096;
    static UBYTE buf[12 * 64];
    ULONG done = 0;
    while (done < n) {
        int k = 0;
        while (k < (int)sizeof buf && done < n) {
            for (int j = 0; j < 6; j++) {
                Disable(); UWORD w = DIAG_DAT; Enable();
                buf[k++] = (UBYTE)(w >> 8);
                buf[k++] = (UBYTE)(w & 0xFF);
            }
            done++;
        }
        Write(f, buf, k);
    }
    Close(f);
    Printf(AUDWOK, (ULONG)n, (ULONG)path);
}

/* Every value a channel was handed at the moment its DMA was switched on.
   Nine of every ten starts on the effects channel make no sound, and one of
   these must say why: a length of nothing, a volume of zero, a pointer into
   the wrong place, or a first word that is already silence. */
/* Which channel is making a sound, and when. One row per frame, oldest first,
   four columns of loudness. A channel that goes quiet exactly when an effect
   should have played shows up here and nowhere else. */
/* What each channel loops BACK to, sampled every 512th loop. A game turns a
   one-shot into silence by pointing the channel at a short quiet buffer; if the
   thing I keep replaying has signal in it, I am looping a piece of the
   effect and that is the drone. */
static void dump_wrap(void)
{
    Disable(); DIAG_SEL = SEL_WRAP; UWORD n = DIAG_DAT; Enable();
    if (!n || n > 32) { PutStr(WRAPNONE); return; }
    PutStr(WRAPHDR);
    for (int i = 0; i < n; i++) {
        ULONG v[4];
        for (int k = 0; k < 4; k++) {
            Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
            v[k] = ((ULONG)h << 16) | l;
        }
        Printf(WRAPROW, v[0], v[1], v[2], v[3]);
    }
}

static void dump_env(void)
{
    Disable(); DIAG_SEL = SEL_ENV; UWORD n = DIAG_DAT; Enable();
    if (!n) { PutStr(ENVNONE); return; }
    PutStr(ENVHDR);
    for (int i = 0; i < n; i++) {
        Disable(); UWORD a = DIAG_DAT, b = DIAG_DAT; Enable();
        ULONG c0 = a >> 8, c1 = a & 0xFF, c2 = b >> 8, c3 = b & 0xFF;
        if (!(c0 | c1 | c2 | c3)) continue;          /* skip silent frames */
        Printf(ENVROW, (ULONG)i, c0, c1, c2, c3);
    }
}

static void dump_trig(void)
{
    Disable(); DIAG_SEL = SEL_TRIG; UWORD n = DIAG_DAT; Enable();
    if (!n || n > 32) { PutStr(TRIGNONE); return; }
    PutStr(TRIGHDR);
    for (int i = 0; i < n; i++) {
        ULONG v[6];
        for (int k = 0; k < 6; k++) {
            Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
            v[k] = ((ULONG)h << 16) | l;
        }
        Printf(TRIGROW, v[0], v[1], v[2], v[3], v[4], v[5]);
    }
}

static void dump_prof(void)
{
    ULONG addr[24], hits[24];
    Disable(); DIAG_SEL = SEL_PROF; Enable();
    for (int i = 0; i < 24; i++) {
        Disable();
        UWORD ah = DIAG_DAT; UWORD al = DIAG_DAT;
        UWORD hh = DIAG_DAT; UWORD hl = DIAG_DAT;
        Enable();
        addr[i] = ((ULONG)ah << 16) | al;
        hits[i] = ((ULONG)hh << 16) | hl;
    }
    ULONG total = 0;
    for (int i = 0; i < 24; i++) total += hits[i];
    if (!total) { PutStr(NOSAMP); return; }
    Printf(PROFHDR, total);
    for (int n = 0; n < 24; n++) {
        int best = -1;
        for (int i = 0; i < 24; i++)
            if (hits[i] && (best < 0 || hits[i] > hits[best])) best = i;
        if (best < 0) break;
        Printf(PROFROW, addr[best], hits[best], (ULONG)((hits[best] * 100) / total));
        hits[best] = 0;
    }
}

static void dump_hist(void)
{
    Disable(); DIAG_SEL = SEL_HIST; Enable();
    ULONG rd[256], wr[256], bv[64];
    for (int i = 0; i < 256; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); rd[i] = ((ULONG)h << 16) | l; }
    for (int i = 0; i < 256; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); wr[i] = ((ULONG)h << 16) | l; }
    for (int i = 0; i < 64; i++)  { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); bv[i] = ((ULONG)h << 16) | l; }
    ULONG tot = 0;
    for (int i = 0; i < 256; i++) tot += rd[i] + wr[i];
    if (!tot) { PutStr("\n--- register histogram: empty (older kernel?) ---\n"); return; }
    PutStr("\n--- registers touched, busiest first ---\n");
    for (int n = 0; n < 12; n++) {
        int best = -1; ULONG bestv = 0;
        for (int i = 0; i < 256; i++) {
            ULONG v = rd[i] + wr[i];
            if (v > bestv) { bestv = v; best = i; }
        }
        if (best < 0 || !bestv) break;
        const char *nm = regname(best << 1);
        char rbuf[8];
        if (!nm) { sprintf(rbuf, "$%03x", best << 1); nm = rbuf; }
        Printf("  %-9s  read %-10ld write %ld\n", (ULONG)nm, rd[best], wr[best]);
        rd[best] = wr[best] = 0;
    }
    /* The top twelve are always the beam, the blitter and INTENA, which hides
       the registers a game touches once a frame - the joystick, the mouse and
       the pots among them. So list every other register it touched too. */
    PutStr("\n--- every other register touched, by address ---\n");
    for (int i = 0; i < 256; i++) {
        if (!rd[i] && !wr[i]) continue;
        const char *nm = regname(i << 1);
        char rbuf[8];
        if (!nm) { sprintf(rbuf, "$%03x", i << 1); nm = rbuf; }
        Printf("  %-9s  read %-10ld write %ld\n", (ULONG)nm, rd[i], wr[i]);
    }
    PutStr("\n--- values the beam reads returned (64 buckets of 1024) ---\n");
    for (int i = 0; i < 64; i++)
        if (bv[i]) Printf("  $%04lx-$%04lx  %ld\n", (ULONG)(i << 10), (ULONG)((i << 10) + 1023), bv[i]);
    ULONG ca[16], cb[16];
    for (int i = 0; i < 16; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); ca[i] = ((ULONG)h << 16) | l; }
    for (int i = 0; i < 16; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); cb[i] = ((ULONG)h << 16) | l; }
    static const char *const cianame[16] = {
        "PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
        "TODLO", "TODMID", "TODHI", "?", "SDR", "ICR", "CRA", "CRB" };
    ULONG pr[256], pw[256];
    for (int i = 0; i < 256; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); pr[i] = ((ULONG)h << 16) | l; }
    for (int i = 0; i < 256; i++) { Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable(); pw[i] = ((ULONG)h << 16) | l; }
    PutStr("\n--- CIA accesses (invisible to every other counter) ---\n");
    for (int i = 0; i < 16; i++) {
        if (ca[i]) Printf("  CIA-A %-7s read %-8ld write %ld\n", (ULONG)cianame[i], ca[i] & 0xFFFF, ca[i] >> 16);
        if (cb[i]) Printf("  CIA-B %-7s read %-8ld write %ld\n", (ULONG)cianame[i], cb[i] & 0xFFFF, cb[i] >> 16);
    }
    /* the CIA access ring comes first in the kernel's layout: 64 entries, one
       word each, delivered as (0, value) pairs. Skipping it silently is what
       made the fetch addresses read as CIA traffic. */
    UWORD ring[64];
    for (int i = 0; i < 64; i++) { Disable(); DIAG_DAT; ring[i] = DIAG_DAT; Enable(); }
    ULONG pa[16], ph[16];
    for (int i = 0; i < 16; i++) {
        Disable(); UWORD ah = DIAG_DAT, al = DIAG_DAT, hh = DIAG_DAT, hl = DIAG_DAT; Enable();
        pa[i] = ((ULONG)ah << 16) | al; ph[i] = ((ULONG)hh << 16) | hl;
    }
    PutStr("\n--- CIA-A port A (OVL,LED,CHNG,WPRO,TK0,RDY,FIR0,FIR1) ---\n");
    for (int i = 0; i < 256; i++)
        if (pr[i] || pw[i]) Printf("  $%02lx  read %-8ld write %ld\n", (ULONG)i, pr[i], pw[i]);
    PutStr("\n--- the last CIA accesses in order, oldest first ---\n");
    for (int i = 0; i < 64; i++) {
        if (!ring[i]) continue;
        Printf("  %s CIA-%s %-7s $%02lx\n", (ULONG)((ring[i] & 0x8000) ? "W" : "R"),
               (ULONG)((ring[i] & 0x4000) ? "A" : "B"),
               (ULONG)cianame[(ring[i] >> 8) & 15], (ULONG)(ring[i] & 0xFF));
    }
    PutStr("\n--- where the 68k executes from chip RAM (JIT line fills) ---\n");
    for (int n = 0; n < 16; n++) {
        int best = -1; ULONG bv = 0;
        for (int i = 0; i < 16; i++) if (ph[i] > bv) { bv = ph[i]; best = i; }
        if (best < 0 || !bv) break;
        Printf("  $%06lx  %ld\n", pa[best], ph[best]);
        ph[best] = 0;
    }
}

static void dump_pcs(void)
{
    Disable(); DIAG_SEL = SEL_PCS; UWORD hdr = DIAG_DAT; Enable();
    int n = hdr & 0xFF;
    if (!n || n > 64) return;
    Printf("\n--- 68k PC, one per frame, oldest first (%s) ---\n",
           (ULONG)((hdr & 0x8000) ? "FROZEN at the game's last display" : "live, not frozen"));
    for (int i = 0; i < n; i++) {
        Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
        Printf("  $%08lx\n", (ULONG)(((ULONG)h << 16) | l));
    }
}

static void dump_regs(void)
{
    Disable();
    DIAG_SEL = SEL_REGS;
    UWORD n = DIAG_DAT;
    Enable();
    if (!n || n > 64) {
        PutStr("--- display state --- not available (older kernel?)\n");
        return;
    }
    UWORD v[64];
    for (int i = 0; i < n; i++) { Disable(); v[i] = DIAG_DAT; Enable(); }
    UWORD st[3];
    for (int i = 0; i < 3; i++) { Disable(); st[i] = DIAG_DAT; Enable(); }
    ULONG bpl[8];
    for (int i = 0; i < 8; i++) {
        Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
        bpl[i] = ((ULONG)h << 16) | l;
    }
    PutStr("--- virtual chipset display state ---\n");
    for (int i = 0; i < n; i++)
        Printf("  %-9s $%04lx\n", (ULONG)(i < (int)REGS_NAMED ? regs_names[i] : "?"), (ULONG)v[i]);
    Printf("  %-9s $%04lx   (accumulated, not last write)\n", (ULONG)"DMACON", (ULONG)st[0]);
    Printf("  %-9s $%04lx\n", (ULONG)"INTENA", (ULONG)st[1]);
    Printf("  %-9s $%04lx\n", (ULONG)"INTREQ", (ULONG)st[2]);
    /* BPLCON0 bits 14-12 are the plane count, bit 4 is the 8-plane bit on AGA */
    ULONG planes = ((v[0] >> 12) & 7) | ((v[0] & 0x0010) ? 8 : 0);
    Printf("\n  bitplanes enabled: %ld   colour depth %ld\n", planes, (ULONG)(1L << planes));
    for (int i = 0; i < 8; i++)
        Printf("  BPL%ldPT    $%08lx\n", (ULONG)(i + 1), bpl[i]);
    if (!planes)
        PutStr("\n  BPLCON0 says NO bitplanes: nothing has set a display up right now.\n");

    /* The high-water snapshot: the last frame anything had bitplanes on. Kept
       in ARM memory outside the chipset struct, so a warm reset - which a
       wedged game forces, and which wipes the live registers - does not lose
       it. This is the part that answers "did the game ever draw anything". */
    UWORD pv[64], ps[3];
    ULONG pb[8];
    int have = 0;
    for (int i = 0; i < n; i++) { Disable(); pv[i] = DIAG_DAT; Enable(); if (pv[i]) have = 1; }
    for (int i = 0; i < 3; i++) { Disable(); ps[i] = DIAG_DAT; Enable(); if (ps[i]) have = 1; }
    for (int i = 0; i < 8; i++) {
        Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
        pb[i] = ((ULONG)h << 16) | l;
        if (pb[i]) have = 1;
    }
    if (!have) {
        PutStr("\n--- high-water: NOTHING has had bitplanes enabled since power-on ---\n"
               "  No game ever set a display up. The renderer is not involved.\n");
        return;
    }
    PutStr("\n--- high-water: the last frame something DID have a display ---\n");
    for (int i = 0; i < n; i++)
        Printf("  %-9s $%04lx\n", (ULONG)(i < (int)REGS_NAMED ? regs_names[i] : "?"), (ULONG)pv[i]);
    Printf("  %-9s $%04lx\n", (ULONG)"DMACON", (ULONG)ps[0]);
    Printf("  %-9s $%04lx\n", (ULONG)"INTENA", (ULONG)ps[1]);
    Printf("  %-9s $%04lx\n", (ULONG)"INTREQ", (ULONG)ps[2]);
    ULONG pp = ((pv[0] >> 12) & 7) | ((pv[0] & 0x0010) ? 8 : 0);
    Printf("\n  bitplanes: %ld   colour depth %ld\n", pp, (ULONG)(1L << pp));
    for (int i = 0; i < 8; i++)
        Printf("  BPL%ldPT    $%08lx\n", (ULONG)(i + 1), pb[i]);
    /* NOT dumped here any more: all of it together overflows what one caffed
       RUN returns, and the sections that mattered were silently cut off.
       agastat HIST / PCS / DEEP print them one at a time. */
}

/* CHIPTEST: can the 68k read back what it writes to chip RAM, at addresses
   across both megabytes? Every WHDLoad game that wants more than 1 MB of chip
   RAM fails under the sandbox and the one that wants 512 KB plays, so the
   second megabyte is suspect. Run this with the sandbox OFF (real chip RAM,
   the baseline) and ON (my virtual copy) and compare. AllocAbs so the OS
   is never trampled; an address the OS is using is reported and skipped. */
static void chiptest(void)
{
    static const ULONG addrs[] = { 0x080000, 0x0F0000, 0x0FF000, 0x100000, 0x101000,
                                   0x140000, 0x180000, 0x1C0000, 0x1F0000, 0x1FF000 };
    const ULONG size = 4096;
    PutStr("--- chip RAM write/read-back from the 68k ---\n");
    for (int i = 0; i < (int)(sizeof addrs / sizeof addrs[0]); i++) {
        ULONG base = addrs[i];
        APTR p = AllocAbs(size, (APTR)base);
        if (!p) { Printf("  $%06lx  in use, skipped\n", base); continue; }
        volatile ULONG *m = (volatile ULONG *)p;
        int bad = 0;
        ULONG first_bad = 0, got = 0, want = 0;
        for (ULONG k = 0; k < size / 4; k++) m[k] = (base + k * 4) ^ 0xA5A5A5A5;
        CacheClearU();
        for (ULONG k = 0; k < size / 4; k++) {
            ULONG w = (base + k * 4) ^ 0xA5A5A5A5, g = m[k];
            if (g != w) { if (!bad) { first_bad = base + k * 4; got = g; want = w; } bad++; }
        }
        if (!bad) Printf("  $%06lx  OK\n", base);
        else      Printf("  $%06lx  BAD: %ld of %ld words wrong, first at $%06lx got $%08lx want $%08lx\n",
                         base, (LONG)bad, (LONG)(size / 4), first_bad, got, want);
        FreeMem(p, size);
    }
}

/* CODETEST: can the 68k EXECUTE code placed in chip RAM across both
   megabytes? CHIPTEST covers data; in a JIT, instruction fetch is a separate
   path, and a game whose code lands above 1 MB would die exactly the way the
   big-BaseMem games do - under the sandbox and on the real chipset alike,
   since the 68k side is the same in both. The routine is
       move.l #$0BADC0DE,d0 ; add.l #<addr>,d0 ; rts
   so the return value proves both that it ran and WHICH copy ran. */
static void codetest(void)
{
    static const ULONG addrs[] = { 0x080000, 0x0F0000, 0x100000, 0x101000,
                                   0x140000, 0x180000, 0x1C0000, 0x1FF000 };
    PutStr("--- execute code placed in chip RAM ---\n");
    for (int i = 0; i < (int)(sizeof addrs / sizeof addrs[0]); i++) {
        ULONG base = addrs[i];
        UWORD *p = AllocAbs(64, (APTR)base);
        if (!p) { Printf("  $%06lx  in use, skipped\n", base); continue; }
        p[0] = 0x203C; p[1] = 0x0BAD; p[2] = 0xC0DE;                 /* move.l #$0BADC0DE,d0 */
        p[3] = 0x0680; p[4] = (UWORD)(base >> 16); p[5] = (UWORD)base; /* addi.l #base,d0     */
        p[6] = 0x4E75;                                                /* rts                 */
        CacheClearU();
        ULONG (*fn)(void) = (ULONG (*)(void))p;
        ULONG got = fn(), want = 0x0BADC0DE + base;
        if (got == want) Printf("  $%06lx  OK\n", base);
        else             Printf("  $%06lx  BAD: returned $%08lx, wanted $%08lx\n", base, got, want);
        FreeMem(p, 64);
    }
}


/* BEAM: what does the 68k see when it polls the beam counters? Every WHDLoad
   game under the sandbox spun on VHPOSR at millions of reads a second and
   never set a display up; a wait for a raster line that never arrives looks
   exactly like that. Read VPOSR/VHPOSR back to back and report whether the
   position moves, how far, and how often the frame wraps. Real chipset:
   ~10 wraps in 200k reads, vpos up to 312, hpos 0..227, LOF flipping. */
static void beamtest(void)
{
    volatile UWORD *vposr = (volatile UWORD *)0xDFF004, *vhposr = (volatile UWORD *)0xDFF006;
    const ULONG N = 200000;
    ULONG changes = 0, wraps = 0, lofflips = 0, first_change = 0;
    ULONG minv = 0xFFFF, maxv = 0, minh = 0xFFFF, maxh = 0;
    ULONG last = 0xFFFFFFFF, lastv = 0; UWORD lastlof = 0;
    UWORD vp0 = *vposr, vh0 = *vhposr;
    for (ULONG i = 0; i < N; i++) {
        UWORD vp = *vposr, vh = *vhposr;
        ULONG both = ((ULONG)vp << 16) | vh;
        ULONG v = ((ULONG)(vp & 7) << 8) | (vh >> 8), h = vh & 0xFF;
        if (both != last) { changes++; if (!first_change) first_change = i + 1; }
        if (i && v < lastv) wraps++;
        if (i && ((vp ^ lastlof) & 0x8000)) lofflips++;
        if (v < minv) minv = v; if (v > maxv) maxv = v;
        if (h < minh) minh = h; if (h > maxh) maxh = h;
        last = both; lastv = v; lastlof = vp;
    }
    Printf("--- beam counters as the 68k sees them (%ld reads) ---\n", N);
    Printf("  first read     VPOSR $%04lx VHPOSR $%04lx\n", (ULONG)vp0, (ULONG)vh0);
    Printf("  last read      VPOSR $%04lx VHPOSR $%04lx\n", (ULONG)(last >> 16), last & 0xFFFF);
    Printf("  value changed  %ld times (first change at read %ld)\n", changes, first_change);
    Printf("  vpos range     %ld..%ld   hpos range %ld..%ld\n", minv, maxv, minh, maxh);
    Printf("  frame wraps    %ld   LOF flips %ld\n", wraps, lofflips);
    if (changes < 2)      PutStr("  => BEAM FROZEN: a wait for a raster line never ends here.\n");
    else if (wraps == 0)  PutStr("  => beam moves but never wraps: a wait for vblank never ends here.\n");
    else                  PutStr("  => beam moves and wraps.\n");
}

/* VBLANK: does the vertical-blank interrupt reach exec? dmatest slept forever
   in WaitTOF under the sandbox, and so does everything that uses the VBLANK
   timer unit (Delay, input.device, WHDLoad's own waits). Hook a VERTB server
   for 2 seconds of CIA time (timer.device MICROHZ, which the sandbox mirrors
   from the real Paula) and count. Real chipset: ~100. */
static volatile ULONG vb_count;
static LONG vb_server(void)
{
    vb_count++;
    return 0;
}
static void vblanktest(void)
{
    struct Interrupt is;
    struct MsgPort *port = CreateMsgPort();
    if (!port) { PutStr("no port\n"); return; }
    struct timerequest *tr = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
    if (!tr) { DeleteMsgPort(port); PutStr("no ioreq\n"); return; }
    if (OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
        PutStr("no timer.device\n"); DeleteIORequest((struct IORequest *)tr); DeleteMsgPort(port); return;
    }
    memset(&is, 0, sizeof is);
    is.is_Node.ln_Type = NT_INTERRUPT; is.is_Node.ln_Pri = -60; is.is_Node.ln_Name = (char *)"agastat vblank";
    is.is_Code = (void (*)())vb_server;
    vb_count = 0;
    AddIntServer(INTB_VERTB, &is);
    tr->tr_node.io_Command = TR_ADDREQUEST; tr->tr_time.tv_secs = 2; tr->tr_time.tv_micro = 0;
    DoIO((struct IORequest *)tr);
    RemIntServer(INTB_VERTB, &is);
    ULONG n = vb_count;
    CloseDevice((struct IORequest *)tr); DeleteIORequest((struct IORequest *)tr); DeleteMsgPort(port);
    Printf("--- VERTB interrupts reaching exec in 2 s of CIA time: %ld ---\n", n);
    if (n == 0)      PutStr("  => NO vertical-blank interrupt: WaitTOF, Delay and the VBLANK timer all hang.\n");
    else if (n < 80) PutStr("  => some, but far fewer than the ~100 expected.\n");
    else             PutStr("  => vertical blank is delivered.\n");
}

/* CIA: the CIA timers and control registers as the 68k sees them, twice,
   about a second apart (busy wait - Delay may be exactly what is broken).
   ICR is NOT read: reading it acknowledges the interrupt. Plus the custom
   INTENAR/INTREQR/DMACONR words; in the sandbox those are the virtual ones
   with the real PORTS/EXTER bits folded into INTREQR. */
static void ciadump(void)
{
    volatile UBYTE *ciaa = (volatile UBYTE *)0xBFE001, *ciab = (volatile UBYTE *)0xBFD000;
    volatile UWORD *custom = (volatile UWORD *)0xDFF000;
    for (int pass = 0; pass < 2; pass++) {
        Printf("--- CIA sample %ld ---\n", (LONG)pass);
        Printf("  CIA-A  CRA $%02lx CRB $%02lx  TA $%02lx%02lx  TB $%02lx%02lx  PRA $%02lx\n",
               (ULONG)ciaa[0xE00], (ULONG)ciaa[0xF00], (ULONG)ciaa[0x500], (ULONG)ciaa[0x400],
               (ULONG)ciaa[0x700], (ULONG)ciaa[0x600], (ULONG)ciaa[0x000]);
        Printf("  CIA-B  CRA $%02lx CRB $%02lx  TA $%02lx%02lx  TB $%02lx%02lx  PRB $%02lx\n",
               (ULONG)ciab[0xE00], (ULONG)ciab[0xF00], (ULONG)ciab[0x500], (ULONG)ciab[0x400],
               (ULONG)ciab[0x700], (ULONG)ciab[0x600], (ULONG)ciab[0x100]);
        Printf("  custom INTENAR $%04lx INTREQR $%04lx DMACONR $%04lx\n",
               (ULONG)custom[0x1C / 2], (ULONG)custom[0x1E / 2], (ULONG)custom[0x02 / 2]);
        if (pass == 0) { volatile ULONG spin = 0; for (ULONG i = 0; i < 3000000; i++) spin += i; }
    }
}

static int check_magic(int *count)
{
    Disable();
    DIAG_SEL = SEL_MAGIC;
    UWORD a = DIAG_DAT, b = DIAG_DAT, n = DIAG_DAT;
    Enable();
    *count = n;
    return a == 0xA6A1 && b == 0xD1A6 && n > 0 && n <= 512;   /* 512: the last selector, $17FF, is counter 511 */
}

static void dump_counters(int count)
{
    for (int i = 0; i < count; i++) {
        ULONG v = read32(i);
        Printf("  %-18s %10lu", (ULONG)(i < (int)NAMED ? names[i] : "?"), v);
        if (i == 15) Printf("   (%s%s%s)",
                            (ULONG)((v & 1) ? "chipset on" : "chipset off"),
                            (ULONG)((v & 2) ? ", sandbox" : ""),
                            (ULONG)((v & 4) ? ", native screen" : ""));
        PutStr("\n");
    }
}

static void dump_log(void)
{
    PutStr("--- log ---\n");
    Disable();
    DIAG_SEL = SEL_LOG;
    Enable();
    char line[256];
    int n = 0, done = 0;
    for (int guard = 0; guard < 4096 && !done; guard++) {
        Disable();
        UWORD w = DIAG_DAT;
        Enable();
        char c[2] = { (char)(w >> 8), (char)(w & 0xFF) };
        for (int k = 0; k < 2; k++) {
            if (!c[k]) { done = 1; break; }
            if (c[k] == '\n' || n >= (int)sizeof line - 1) {
                line[n] = 0;
                if (n) { PutStr(line); PutStr("\n"); }
                n = 0;
                if (c[k] != '\n') line[n++] = c[k];
            } else {
                line[n++] = c[k];
            }
        }
    }
    if (n) { line[n] = 0; PutStr(line); PutStr("\n"); }
}

/* The screen as the last soft reset found it, for agahost on the PC: the
   kernel kept the chip RAM (it does not clear it when a session was kept)
   and the chipset registers and palette. Written to DIR (default RAM:snap)
   as chipmem.bin, regs.bin and colors.bin - the WinUAE snapshot format. */
static int prevsnap(const char *dir)
{
    char path[256];
    UBYTE *chip = AllocMem(2 * 1024 * 1024, MEMF_ANY);
    UWORD regs[256];
    ULONG colors[256];
    BPTR f;
    if (!chip) { PutStr("agastat: no memory for 2 MB\n"); return 20; }
    /* in pieces, so the machine is never deaf for long */
    /* the selection stays put between the pieces: only agastat and agaboot
       ever select, and neither runs alongside this */
    DIAG_SEL = SEL_PCHIP;
    for (ULONG base = 0; base < 1024 * 1024; base += 16384) {
        Disable();
        for (ULONG i = 0; i < 16384; i++) ((UWORD *)chip)[base + i] = DIAG_DAT;
        Enable();
    }
    Disable();
    DIAG_SEL = SEL_PREGS;
    for (int i = 0; i < 256; i++) regs[i] = DIAG_DAT;
    for (int i = 0; i < 256; i++) { ULONG hi = DIAG_DAT, lo = DIAG_DAT; colors[i] = (hi << 16) | lo; }
    Enable();
    UnLock(CreateDir((STRPTR)dir));
    sprintf(path, "%s/chipmem.bin", dir);
    if (!(f = Open((STRPTR)path, MODE_NEWFILE))) { FreeMem(chip, 2 * 1024 * 1024); PutStr("agastat: cannot write\n"); return 20; }
    Write(f, chip, 2 * 1024 * 1024); Close(f);
    sprintf(path, "%s/regs.bin", dir);
    if ((f = Open((STRPTR)path, MODE_NEWFILE))) { Write(f, regs, sizeof regs); Close(f); }
    sprintf(path, "%s/colors.bin", dir);
    if ((f = Open((STRPTR)path, MODE_NEWFILE))) { Write(f, colors, sizeof colors); Close(f); }
    FreeMem(chip, 2 * 1024 * 1024);
    Printf("snapshot written to %s: BPLCON0 $%04lx FMODE $%04lx DMACON $%04lx BPL1PT $%04lx%04lx COP1LC $%04lx%04lx\n",
           (ULONG)dir, (ULONG)regs[0x100 / 2], (ULONG)regs[0x1FC / 2], (ULONG)regs[0x096 / 2],
           (ULONG)regs[0x0E0 / 2], (ULONG)regs[0x0E2 / 2], (ULONG)regs[0x080 / 2], (ULONG)regs[0x082 / 2]);
    return 0;
}

/* The kept 82-second profile and the code around its hot spots, raw, for
   the PC to take apart. */
static int prevprof(const char *file)
{
    UWORD *buf = AllocMem(PPROF_WORDS * 2, MEMF_ANY);
    BPTR f;
    if (!buf) return 20;
    DIAG_SEL = SEL_PPROF;
    for (ULONG i = 0; i < PPROF_WORDS; i++) buf[i] = DIAG_DAT;
    if ((f = Open((STRPTR)file, MODE_NEWFILE))) { Write(f, buf, PPROF_WORDS * 2); Close(f); }
    FreeMem(buf, PPROF_WORDS * 2);
    Printf("profile written to %s\n", (ULONG)file);
    return 0;
}

int main(int argc, char **argv)
{
    int count = 0;
    /* needs no diag interface at all - it only talks to chip RAM */
    /* Every run starts on THIS session; PREV switches the kernel over to the
       one kept across the last soft reset and then runs the ordinary dump, so
       "agastat PREV" is the counters and log and "agastat PREV TRACE" the
       frame trace of the session that hung. */
    Disable(); DIAG_SEL = SEL_PREV_OFF; Enable();
    if (argc > 1 && !stricmp(argv[1], "PREV")) {
        Disable(); DIAG_SEL = SEL_PREV_Q; UWORD have = DIAG_DAT; Enable();
        if (!have) { PutStr("agastat: no session was kept across the last reset\n"); return 5; }
        Disable(); DIAG_SEL = SEL_PREV_ON; Enable();
        PutStr("=== the session before the last reset ===\n");
        argc--; argv++;
    }
    if (argc > 1 && !stricmp(argv[1], "PREVPROF")) {
        Disable(); DIAG_SEL = SEL_PREV_Q; UWORD have = DIAG_DAT; Enable();
        if (!have) return 5;
        return prevprof(argc > 2 ? argv[2] : "RAM:prof.bin");
    }
    if (argc > 1 && !stricmp(argv[1], "PREVSNAP")) {
        Disable(); DIAG_SEL = SEL_PREV_Q; UWORD have = DIAG_DAT; Enable();
        if (!have) { PutStr("agastat: no session was kept across the last reset\n"); return 5; }
        return prevsnap(argc > 2 ? argv[2] : "RAM:snap");
    }
    if (argc > 1 && !stricmp(argv[1], "CHIPTEST")) { chiptest(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "CODETEST")) { codetest(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "BEAM"))     { beamtest();   return 0; }
    if (argc > 1 && !stricmp(argv[1], "VBLANK"))   { vblanktest(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "CIA"))      { ciadump();    return 0; }
    if (!check_magic(&count)) {
        PutStr("agastat: no AGA-PISTORM diagnostics here.\n"
               "  Either this is a stock Emu68, or the AGA kernel is not the one running.\n");
        return 10;
    }
    if (argc > 1 && !stricmp(argv[1], "TRAPS")) { dump_traps(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "REGS"))  { dump_regs();  return 0; }
    if (argc > 1 && !stricmp(argv[1], "HIST"))  { dump_hist();  return 0; }
    if (argc > 1 && !stricmp(argv[1], "SERIAL")) { dump_serial(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "CODE"))   { dump_code(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "PROFILE")) { dump_prof(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "TRIG")) { dump_trig(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "ENV")) { dump_env(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "WRAP")) { dump_wrap(); return 0; }
    if (argc > 1 && !stricmp(argv[1], "AUDW")) {
        dump_audw(argc > 2 ? (const char *)argv[2] : "T:audw.bin");
        return 0;
    }
    if (argc > 1 && !stricmp(argv[1], "WAVE")) {
        dump_wave(argc > 2 ? (const char *)argv[2] : "T:aga_dac.raw");
        return 0;
    }
    /* SEQ, not BEAM: BEAM above polls the beam live from the CLI, this reads
       back the values the kernel handed a game that was stuck in a raster wait */
    if (argc > 1 && !stricmp(argv[1], "TRACE")) {
        Disable(); DIAG_SEL = SEL_TRACE; Enable();
        PutStr("--- the last 64 frames, oldest first ---\n"
               "   68k SR  traps  chipwrites  irq3  irq6   jit units        PC\n");
        for (int i = 0; i < 64; i++) {
            Disable();
            UWORD sr = DIAG_DAT, tr = DIAG_DAT, cw = DIAG_DAT, ir = DIAG_DAT;
            UWORD th = DIAG_DAT, tl = DIAG_DAT, ph = DIAG_DAT, pl = DIAG_DAT;
            Enable();
            Printf("   $%04lx  %5ld  %10ld  %4ld  %4ld  %9ld  $%08lx\n",
                   (ULONG)sr, (ULONG)tr, (ULONG)cw, (ULONG)(ir >> 8), (ULONG)(ir & 0xFF),
                   ((ULONG)th << 16) | tl, ((ULONG)ph << 16) | pl);
        }
        return 0;
    }
    if (argc > 1 && !stricmp(argv[1], "CODE2")) {
        Disable(); DIAG_SEL = SEL_CODE2; UWORD h2 = DIAG_DAT; Enable();
        Disable(); UWORD l2 = DIAG_DAT; Enable();
        ULONG base = ((ULONG)h2 << 16) | l2;
        Printf("--- 68k code at $%08lx (where the last interrupt found the CPU) ---\n", base);
        for (int r = 0; r < 16; r++) {
            Printf("$%08lx ", base + r * 16);
            for (int c = 0; c < 8; c++) {
                Disable(); UWORD w = DIAG_DAT; Enable();
                Printf("%04lx ", (ULONG)w);
            }
            PutStr("\n");
        }
        return 0;
    }
    if (argc > 1 && !stricmp(argv[1], "SEQ")) {
        Disable(); DIAG_SEL = SEL_BEAM; Enable();
        PutStr(BEAMHDR);
        LONG prev = -1;
        for (int i = 0; i < 96; i++) {
            Disable(); UWORD comp = DIAG_DAT, us = DIAG_DAT; Enable();
            int hp = (comp & 0x4000) != 0, pr = (comp & 0x8000) != 0;
            LONG line = pr ? (comp & 0x1FF) : (comp & 0xFF);
            Printf(BEAMROW, (ULONG)(pr ? "pair" : hp ? "hpos" : "line"), line, (ULONG)us, prev < 0 ? 0 : line - prev);
            prev = line;
        }
        /* which line numbers I handed out at all since the display went blank:
           if a stuck raster wait is waiting for a line that is in here, the
           values are reaching it and the fault is elsewhere */
        ULONG cov[32];
        for (int i = 0; i < 32; i++) {
            Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
            cov[i] = ((ULONG)h << 16) | l;
        }
        Disable(); UWORD bor = DIAG_DAT, band = DIAG_DAT; Enable();
        for (int half = 0; half < 3; half++) {
        ULONG *c = cov + half * 8;
        int n = half == 2 ? 512 : 256;
        if (half == 2) c = cov + 16;
        Printf(COVWHAT, (ULONG)(half == 0 ? "line low bytes ($DFF006)"
                             : half == 1 ? "positions along a line ($DFF007)"
                             : "whole line numbers from a $DFF005+$DFF006 pair"));
        int seen = 0, first_missing = -1, last_seen = -1;
        for (int i = 0; i < n; i++) if (c[i >> 5] & (1UL << (i & 31))) { seen++; last_seen = i; }
        PutStr(COVLIST);
        int runs = 0;
        for (int i = 0; i < n && runs < 12; i++) {
            if (c[i >> 5] & (1UL << (i & 31))) continue;
            int j = i;
            while (j < n && !(c[j >> 5] & (1UL << (j & 31)))) j++;
            Printf(COVGAP, (LONG)i, (LONG)(j - 1));
            runs++; i = j;
        }
        Printf(COVROW, (LONG)seen, (LONG)n, (LONG)first_missing);
        }
        Printf(COVBITS, (ULONG)bor, (ULONG)band);
        return 0;
    }
    if (argc > 1 && !stricmp(argv[1], "PCE")) {
        Disable(); DIAG_SEL = SEL_PCE; Enable();
        PutStr(PCEHDR);
        for (int i = 0; i < 32; i++) {
            Disable(); UWORD h = DIAG_DAT, l = DIAG_DAT; Enable();
            Printf(PCEROW, ((ULONG)h << 16) | l);
        }
        return 0;
    }
    if (argc > 1 && !stricmp(argv[1], "PCS"))   { dump_pcs();   return 0; }
    if (argc > 1 && !stricmp(argv[1], "DEEP"))  { dump_deep();  return 0; }
    int want_log = argc > 1 && !stricmp(argv[1], "LOG");
    int watch    = argc > 1 && !stricmp(argv[1], "WATCH");
    if (watch) {
        for (;;) {
            PutStr("--- counters ---\n");
            dump_counters(count);
            if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) break;
            Delay(50);
        }
        return 0;
    }
    if (!want_log) {
        Printf("AGA-PISTORM diagnostics, %ld counters\n", (LONG)count);
        dump_counters(count);
    }
    dump_log();
    return 0;
}
