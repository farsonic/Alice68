/* SPDX-License-Identifier: GPL-2.0-only
 * AmigaOS library calls from the m68k ELF gcc, without register variables
 * (gcc reused a pinned register under -O2 and corrupted a stack address):
 *   ULONG lvo_call(APTR base, LONG offset, ULONG d0, ULONG d1, ULONG a0, ULONG a1)
 *   LONG  dos_write(APTR dosbase, ULONG fh, const void *buf, LONG len)   -48(a6): d1/d2/d3
 * Arguments on the stack (gcc), registers loaded here; d2-d7/a2-a6 kept. */
        .text
        .globl  lvo_call
lvo_call:
        movem.l %d2/%a6, -(%sp)           /* 8 bytes */
        move.l  12(%sp), %a6              /* base */
        move.l  16(%sp), %d2              /* offset */
        move.l  20(%sp), %d0
        move.l  24(%sp), %d1
        move.l  28(%sp), %a0
        move.l  32(%sp), %a1
        jsr     0(%a6,%d2.l)
        movem.l (%sp)+, %d2/%a6
        rts

        .globl  dos_write
dos_write:
        movem.l %d2-%d3/%a6, -(%sp)       /* 12 bytes */
        move.l  16(%sp), %a6
        move.l  20(%sp), %d1
        move.l  24(%sp), %d2
        move.l  28(%sp), %d3
        jsr     -48(%a6)
        movem.l (%sp)+, %d2-%d3/%a6
        rts

/* ULONG lvo_d1(APTR base, LONG offset, ULONG d1) - dos.library Delay() and the like */
        .globl  lvo_d1
lvo_d1:
        movem.l %d2/%a6, -(%sp)
        move.l  12(%sp), %a6
        move.l  16(%sp), %d2
        move.l  20(%sp), %d1
        jsr     0(%a6,%d2.l)
        movem.l (%sp)+, %d2/%a6
        rts
