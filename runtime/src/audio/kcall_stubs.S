/*
 * Imports of the me-core library's kernel helper (kcall.prx).
 *
 * libme-core ships these as old-style stubs whose library header nothing
 * references, so --gc-sections drops it and the calls are never linked.
 * These new-style stubs reference their header, and since they define the
 * same functions, the library's copy isn't pulled in.
 */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "kcall",0x40090000
	IMPORT_FUNC  "kcall",0x75F43FF0,kcall_2
	IMPORT_FUNC  "kcall",0xBADD8D3B,kcall_3
	IMPORT_FUNC  "kcall",0x2BB46CB6,kinit
