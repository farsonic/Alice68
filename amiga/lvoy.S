/* SPDX-License-Identifier: GPL-2.0-only
 * lvo_y(): an AmigaOS library call loading d0-d3 and a0-a3 (a6 = base),
 * for the calls lvo_x() cannot make (GadTools' a2/a3 arguments). */
/* ULONG lvo_y(APTR base, LONG off, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG a0, ULONG a1, ULONG a2, ULONG a3) */
        .text
        .globl  lvo_y
lvo_y:
        movem.l %d2-%d3/%a2-%a3/%a5-%a6, -(%sp)  /* 24 bytes */
        move.l  28(%sp), %a6
        move.l  32(%sp), %d0
        lea     0(%a6,%d0.l), %a5
        move.l  36(%sp), %d0
        move.l  40(%sp), %d1
        move.l  44(%sp), %d2
        move.l  48(%sp), %d3
        move.l  52(%sp), %a0
        move.l  56(%sp), %a1
        move.l  60(%sp), %a2
        move.l  64(%sp), %a3
        jsr     (%a5)
        movem.l (%sp)+, %d2-%d3/%a2-%a3/%a5-%a6
        rts
