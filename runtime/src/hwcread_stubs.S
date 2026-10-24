/*
 * Imports of hwcread.prx (runtime/kmod), the counter-reading kernel module
 * hwcount.c loads when asked for counters. New-style stubs, as in
 * audio/kcall_stubs.S, so --gc-sections keeps them.
 */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "hwcread",0x40090000
	IMPORT_FUNC  "hwcread",0x88C8385C,hwcRead
