/*
 * preempt.c -- lets a long stretch of recompiled code be interrupted.
 *
 * On the N64 the OS preempts a thread wherever it happens to be, so the audio
 * manager always gets its turn every retrace. Our game threads hand over only
 * where the runtime is entered: at OS calls, and inside recompiled code at
 * get_function(), which game code reaches only on an indirect call. Loading a
 * village takes the game through some 800 ms of work with a single indirect
 * call in it, and everything else waited behind all of it -- about fifty
 * retraces' worth of sound, never generated.
 *
 * scripts/recompile.sh therefore opens every recompiled function with
 * RECOMP_PREEMPT() (recomp_psp.h), which costs a byte load and a branch until
 * the scheduler asks for a hand-over. The longest stretch without one is then
 * a single function body, which the stats line reports as `h`.
 */
#include "rt.h"

void __attribute__((noinline)) rt_preempt(void) {
    g_preempt_hint = 0;
    rt_process_external();
    rt_check_preempt();
}
