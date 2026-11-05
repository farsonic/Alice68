/* SPDX-License-Identifier: GPL-2.0-only */
        .section .text.start, "ax"
        .globl  _start
_start:
        movem.l %d2-%d7/%a2-%a6, -(%sp)
        move.l  %d0, -(%sp)
        move.l  %a0, -(%sp)
        jsr     hdfinfo_main
        addq.l  #8, %sp
        movem.l (%sp)+, %d2-%d7/%a2-%a6
        rts
