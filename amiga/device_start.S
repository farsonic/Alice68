/* SPDX-License-Identifier: GPL-2.0-only
 * alice68-usb.device: entry, ROMTag, auto-init table and the register ->
 * C (stack) stubs. AmigaOS passes arguments in registers; the m68k ELF gcc
 * takes them on the stack and saves d2-d7/a2-a6 itself, as AmigaOS wants. */
        .section .text.start, "ax"
        .globl  _start
_start:
        moveq   #-1, %d0                /* not runnable */
        rts

romtag:
        .short  0x4afc                  /* RTC_MATCHWORD */
        .long   romtag
        .long   endskip
        .byte   0x80                    /* RTF_AUTOINIT */
        .byte   1                       /* version */
        .byte   3                       /* NT_DEVICE */
        .byte   0                       /* priority */
        .long   dev_name
        .long   dev_idstring
        .long   dev_inittab

/* d0 = device base, a0 = seglist, a6 = SysBase */
        .globl  stub_init, stub_open, stub_close, stub_expunge, stub_null, stub_beginio, stub_abortio
stub_init:
        move.l  %a0, -(%sp)
        move.l  %d0, -(%sp)
        jsr     dev_init
        addq.l  #8, %sp
        rts

/* a6 = base, a1 = ioreq, d0 = unit, d1 = flags */
stub_open:
        move.l  %d1, -(%sp)
        move.l  %d0, -(%sp)
        move.l  %a1, -(%sp)
        move.l  %a6, -(%sp)
        jsr     dev_open
        lea     16(%sp), %sp
        rts

stub_close:
        move.l  %a1, -(%sp)
        move.l  %a6, -(%sp)
        jsr     dev_close
        addq.l  #8, %sp
        rts

stub_expunge:
        move.l  %a6, -(%sp)
        jsr     dev_expunge
        addq.l  #4, %sp
        rts

stub_null:
        moveq   #0, %d0
        rts

stub_beginio:
        move.l  %a1, -(%sp)
        move.l  %a6, -(%sp)
        jsr     dev_beginio
        addq.l  #8, %sp
        rts

stub_abortio:
        move.l  %a1, -(%sp)
        move.l  %a6, -(%sp)
        jsr     dev_abortio
        addq.l  #8, %sp
        rts

/* interrupt server: a1 = is_Data; return with Z set so the chain goes on */
        .globl  stub_irq
stub_irq:
        movem.l %d2-%d7/%a2-%a6, -(%sp)
        move.l  %a1, -(%sp)
        jsr     dev_irq
        addq.l  #4, %sp
        movem.l (%sp)+, %d2-%d7/%a2-%a6
        moveq   #0, %d0
        rts

        .globl  endskip
endskip:
