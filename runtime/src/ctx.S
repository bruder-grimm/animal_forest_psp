/*
 * ctx.S -- register contexts for game threads (sched.c, capture.c).
 *
 *   int rt_ctx_save(RtCtx* c)           saves what a call keeps (s0-s7, fp, sp, ra,
 *                                       gp, f20-f31, the FPU control word) and
 *                                       returns 0; returns 1 again each time
 *                                       rt_ctx_jump(c) is called, like setjmp
 *   void rt_ctx_jump(const RtCtx* c)    continues at that rt_ctx_save, on any
 *                                       PSP thread, like longjmp
 *   void rt_call_on_stack(void* top, void (*fn)(void*), void* arg)
 *                                       calls fn(arg) with the stack pointer at
 *                                       top, and returns with the caller's
 *
 * Game threads run on stacks of the runtime's own (sched.c), and every one of
 * them that is not running has its context saved where it waits. A capture
 * holds those stacks and contexts, and resuming one jumps new PSP threads
 * back into them.
 *
 * RtCtx (rt.h): s0-s7 at 0, fp 32, sp 36, ra 40, gp 44, f20-f31 at 48, fcr31 at 96.
 */

	.set noreorder
	.set noat
	.text
	.align 2

	.global rt_ctx_save
	.ent rt_ctx_save
rt_ctx_save:
	sw	$s0, 0($a0)
	sw	$s1, 4($a0)
	sw	$s2, 8($a0)
	sw	$s3, 12($a0)
	sw	$s4, 16($a0)
	sw	$s5, 20($a0)
	sw	$s6, 24($a0)
	sw	$s7, 28($a0)
	sw	$fp, 32($a0)
	sw	$sp, 36($a0)
	sw	$ra, 40($a0)
	sw	$gp, 44($a0)
	swc1	$f20, 48($a0)
	swc1	$f21, 52($a0)
	swc1	$f22, 56($a0)
	swc1	$f23, 60($a0)
	swc1	$f24, 64($a0)
	swc1	$f25, 68($a0)
	swc1	$f26, 72($a0)
	swc1	$f27, 76($a0)
	swc1	$f28, 80($a0)
	swc1	$f29, 84($a0)
	swc1	$f30, 88($a0)
	swc1	$f31, 92($a0)
	cfc1	$t0, $31
	sw	$t0, 96($a0)
	jr	$ra
	move	$v0, $zero
	.end rt_ctx_save

	.global rt_ctx_jump
	.ent rt_ctx_jump
rt_ctx_jump:
	lw	$t0, 96($a0)
	ctc1	$t0, $31
	lwc1	$f20, 48($a0)
	lwc1	$f21, 52($a0)
	lwc1	$f22, 56($a0)
	lwc1	$f23, 60($a0)
	lwc1	$f24, 64($a0)
	lwc1	$f25, 68($a0)
	lwc1	$f26, 72($a0)
	lwc1	$f27, 76($a0)
	lwc1	$f28, 80($a0)
	lwc1	$f29, 84($a0)
	lwc1	$f30, 88($a0)
	lwc1	$f31, 92($a0)
	lw	$s0, 0($a0)
	lw	$s1, 4($a0)
	lw	$s2, 8($a0)
	lw	$s3, 12($a0)
	lw	$s4, 16($a0)
	lw	$s5, 20($a0)
	lw	$s6, 24($a0)
	lw	$s7, 28($a0)
	lw	$fp, 32($a0)
	lw	$sp, 36($a0)
	lw	$ra, 40($a0)
	lw	$gp, 44($a0)
	jr	$ra
	li	$v0, 1
	.end rt_ctx_jump

	.global rt_call_on_stack
	.ent rt_call_on_stack
rt_call_on_stack:
	addiu	$sp, $sp, -16
	sw	$ra, 12($sp)
	sw	$s0, 8($sp)
	move	$s0, $sp
	addiu	$sp, $a0, -16		/* the 16 bytes a callee may keep its arguments in */
	move	$t9, $a1
	jalr	$t9
	move	$a0, $a2
	move	$sp, $s0
	lw	$s0, 8($sp)
	lw	$ra, 12($sp)
	jr	$ra
	addiu	$sp, $sp, 16
	.end rt_call_on_stack
