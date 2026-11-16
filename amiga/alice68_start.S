/* SPDX-License-Identifier: GPL-2.0-only
   C:Alice68 - the Shell command: a0 = the argument string, d0 = its length (dos's convention) */
        .section .text.start, "ax"
        .globl  _start
_start:
        movem.l %d2-%d7/%a2-%a6, -(%sp)
        move.l  %d0, -(%sp)             /* argument length */
        move.l  %a0, -(%sp)             /* argument string */
        jsr     alice68_main
        addq.l  #8, %sp
        movem.l (%sp)+, %d2-%d7/%a2-%a6
        rts

/* NETCHECK's SANA-II buffer copy hook (S2_CopyToBuff / S2_CopyFromBuff): a0 = to, a1 = from,
   d0 = bytes; returns d0 = TRUE. The tool's own buffers are plain memory. */
        .text
        .globl  nc_copybuff
nc_copybuff:
        tst.l   %d0
        beq.s   2f
1:      move.b  (%a1)+, (%a0)+
        subq.l  #1, %d0
        bne.s   1b
2:      moveq   #1, %d0
        rts
