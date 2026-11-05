/* SPDX-License-Identifier: GPL-2.0-only
 * lvo_x(): an AmigaOS library call loading d0-d3, a0 and a1 (a6 = base). */
/* ULONG lvo_x(APTR base, LONG off, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG a0, ULONG a1) */
        .text
        .globl  lvo_x
lvo_x:
        movem.l %d2-%d3/%a5-%a6, -(%sp)  /* 16 bytes */
        move.l  20(%sp), %a6
        move.l  24(%sp), %d0
        lea     0(%a6,%d0.l), %a5
        move.l  28(%sp), %d0
        move.l  32(%sp), %d1
        move.l  36(%sp), %d2
        move.l  40(%sp), %d3
        move.l  44(%sp), %a0
        move.l  48(%sp), %a1
        jsr     (%a5)
        movem.l (%sp)+, %d2-%d3/%a5-%a6
        rts

