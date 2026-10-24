/*
 * hwcread.prx -- reads the Allegrex's performance counters for the runtime
 * (runtime/src/hwcount.c). They are kernel addresses (0xBC400000); kcall.prx
 * could reach them, but it invalidates the whole I-cache on every call and
 * the live game hung when its threads used it. Debug only: not shipped.
 * Build: gmake -C runtime/kmod (needs PSPDEV); copy hwcread.prx next to the PRX.
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman.h>

PSP_MODULE_INFO("hwcread", 0x1006, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

#define REGS ((volatile unsigned int*)0xBC400000)
#define WORDS 21

/* Turns counting on, and copies the 21 counter words to out (user memory). */
int hwcRead(unsigned int* out) {
    int k1 = pspSdkSetK1(0);
    int intr = sceKernelCpuSuspendIntr(); /* all 21 from the same moment */
    REGS[0] = 1;
    for (int i = 0; i < WORDS; i++) {
        out[i] = REGS[i];
    }
    sceKernelCpuResumeIntr(intr);
    pspSdkSetK1(k1);
    return 0;
}

int module_start(SceSize args, void* argp) {
    return 0;
}

int module_stop(SceSize args, void* argp) {
    return 0;
}
