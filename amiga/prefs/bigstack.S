| Alice68Prefs: run prefs_main() on a stack of its own when the one it was
| started with is smaller (a Shell gives 4 KB unless told otherwise; MUI's
| layout of the window's nested groups, with our classes' drawing, needs more).
| SPDX-License-Identifier: GPL-2.0-only
	.set	STACK, 65536
	.text
	.globl	amiga_main
| long amiga_main(char *args, long len)
amiga_main:
	movem.l	%d2-%d4/%a2-%a3/%a6,-(%sp)
	move.l	28(%sp),%d2
	move.l	32(%sp),%d3
	move.l	4.w,%a6
	sub.l	%a1,%a1
	jsr	-294(%a6)		| FindTask(NULL)
	move.l	%d0,%a0
	move.l	62(%a0),%d0		| tc_SPUpper
	sub.l	58(%a0),%d0		| - tc_SPLower
	cmp.l	#STACK,%d0
	bcc.s	1f
	move.l	#STACK,%d0
	moveq	#0,%d1
	jsr	-198(%a6)		| AllocMem(STACK, MEMF_ANY)
	tst.l	%d0
	beq.s	1f
	move.l	%d0,%a3
	move.l	%d0,%a2			| struct StackSwapStruct at the start
	lea	16(%a3),%a0
	move.l	%a0,(%a2)		| stk_Lower
	move.l	%a3,%a0
	add.l	#STACK,%a0
	move.l	%a0,4(%a2)		| stk_Upper
	move.l	%a0,8(%a2)		| stk_Pointer
	move.l	%a2,%a0
	jsr	-732(%a6)		| StackSwap: on the new stack
	move.l	%d3,-(%sp)
	move.l	%d2,-(%sp)
	jsr	prefs_main
	addq.l	#8,%sp
	move.l	%d0,%d4
	move.l	4.w,%a6
	move.l	%a2,%a0
	jsr	-732(%a6)		| StackSwap: back
	move.l	%a3,%a1
	move.l	#STACK,%d0
	jsr	-210(%a6)		| FreeMem
	move.l	%d4,%d0
	bra.s	2f
1:	move.l	%d3,-(%sp)
	move.l	%d2,-(%sp)
	jsr	prefs_main
	addq.l	#8,%sp
2:	movem.l	(%sp)+,%d2-%d4/%a2-%a3/%a6
	rts
	.section .note.GNU-stack,"",@progbits
