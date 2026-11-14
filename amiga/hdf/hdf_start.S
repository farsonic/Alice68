/* SPDX-License-Identifier: GPL-2.0-only
 * alice68hdf.device: ROMTag and register -> C stubs. It is loaded by the
 * kernel into Emu68's ROM board (read-only), whose boot code starts every
 * resident it finds there by priority: 10, after brcm-emmc.device (20). */
        .section .text.start, "ax"
        .globl  _start
_start:
        moveq   #-1, %d0
        rts

romtag:
        .short  0x4afc
        .long   romtag
        .long   endskip
        .byte   0x81                    /* RTF_AUTOINIT | RTF_COLDSTART */
        .byte   1                       /* version */
        .byte   3                       /* NT_DEVICE */
        .byte   10                      /* priority */
        .long   hdf_devname
        .long   hdf_idstring
        .long   dev_inittab

        .globl  stub_init, stub_open, stub_close, stub_expunge, stub_null, stub_beginio, stub_abortio
stub_init:                              /* d0 = base, a0 = seglist */
        move.l  %a0, -(%sp)
        move.l  %d0, -(%sp)
        jsr     dev_init
        addq.l  #8, %sp
        rts
stub_open:                              /* a1 = io, d0 = unit, d1 = flags */
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
        moveq   #0, %d0                 /* in ROM: never goes */
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

/* the I/O task: its base is in tc_UserData */
        .globl  task_entry
task_entry:
        move.l  4.w, %a6
        sub.l   %a1, %a1
        jsr     -294(%a6)               /* FindTask(NULL) */
        move.l  %d0, %a0
        move.l  88(%a0), -(%sp)         /* tc_UserData */
        jsr     hdf_task
        addq.l  #4, %sp
        rts

/* void add_task(struct Task *t, APTR initpc): AddTask wants a2/a3 */
        .globl  add_task
add_task:
        movem.l %a2-%a3/%a6, -(%sp)     /* 12 bytes */
        move.l  16(%sp), %a1
        move.l  20(%sp), %a2
        sub.l   %a3, %a3
        move.l  4.w, %a6
        jsr     -282(%a6)
        movem.l (%sp)+, %a2-%a3/%a6
        rts

        .globl  endskip
endskip:
