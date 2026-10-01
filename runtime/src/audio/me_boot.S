/*
 * me_boot.S -- the first thing the Media Engine runs after a reset.
 *
 * me_audio.c copies this to the ME's reset vector with the me-core library's
 * start-up code right behind it, which it runs into.
 *
 * The library's code goes straight on to cached memory. That is fine the
 * first time, when the ME has just been running the firmware's own code and
 * its caches hold what that left there. After a standby the ME has been
 * without power: its cache tags are whatever the memory came up with, and a
 * line that happens to look valid and written-to is "written back" to
 * whatever address its tag spells, anywhere in RAM, the first time the line
 * is reused or the caches are flushed. That froze the PSP shortly after it
 * woke up. So both caches are emptied here first, the way the firmware's own
 * ME reset code and its cache-clearing calls do it (index store tag with a
 * zero tag, for both ways of every line).
 */

	.set noreorder
	.set noat
	.text
	.align 2
	.global rt_me_boot
	.global rt_me_boot_end
	.ent rt_me_boot
rt_me_boot:
	mfc0	$t0, $16		/* Config: the cache sizes */
	ext	$t1, $t0, 9, 3
	li	$t6, 4096
	sllv	$t6, $t6, $t1		/* bytes in a way of the instruction cache */
	ext	$t1, $t0, 6, 3
	li	$t7, 4096
	sllv	$t7, $t7, $t1		/* ... and of the data cache */

	/* empty both caches */
	mtc0	$zero, $28
	mtc0	$zero, $29
	move	$t0, $t6
1:	addiu	$t0, $t0, -64
	cache	0x01, 0($t0)
	bnez	$t0, 1b
	cache	0x03, 0($t0)
	move	$t0, $t7
2:	addiu	$t0, $t0, -64
	cache	0x11, 0($t0)
	bnez	$t0, 2b
	cache	0x13, 0($t0)
	.end rt_me_boot
rt_me_boot_end:
