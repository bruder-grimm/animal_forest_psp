/*
 * log.c -- the runtime log.
 *
 * There is a log only if log.txt (or sync_log.txt) is next to the EBOOT: a
 * game being played has no use for a line of statistics on its memory stick
 * every few seconds. Without it rt_log does nothing; only a fatal stop still
 * leaves its reason in afpsp.log.
 *
 * The log goes to afpsp.log next to the EBOOT and to stdout (which PSPLink
 * shows in its shell). rt_log only puts the line in a buffer; a thread of its
 * own writes the buffer out, so that nobody who logs waits for the memory
 * stick: a write there takes 10-50 ms, at times 300 ms, and the periodic
 * statistics lines alone stopped the renderer for a frame or two every four
 * seconds, and the game's scheduler thread -- which dispatches the sound --
 * every ten.
 *
 * Every write is its own open, append and close: the memory stick's directory
 * entry only learns a file's size when the file is closed, so a crash loses
 * at most the lines still in the buffer, and no handle is left open for a
 * standby to invalidate. With sync_log.txt next to the EBOOT every line is
 * written before rt_log returns, for a crash that takes the buffer with it.
 */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdarg.h>
#include <stdio.h>

#include "rt.h"

#define LOG_BUFFER 32768 /* a power of two */
#define LOG_CHUNK 4096   /* written per open */

static bool sLogOk = false;
static char sBuffer[LOG_BUFFER];
static uint32_t sHead = 0;       /* bytes put in so far */
static uint32_t sTail = 0;       /* bytes written out so far */
static SceUID sBufferLock = -1;  /* guards sBuffer, sHead and sTail */
static SceUID sWriteLock = -1;   /* held while a chunk is taken and written: keeps the file in order */
static SceUID sPending = -1;     /* wakes the writer */
static bool sSync = false;       /* sync_log.txt: every line is written before rt_log returns */

static SceUID open_log(int mode) {
    char path[256];
    return sceIoOpen(rt_data_path("afpsp.log", path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | mode, 0777);
}

static void write_out(const char* text, int len) {
    fwrite(text, 1, (size_t)len, stdout);
    if (sLogOk) {
        rt_io_begin();
        SceUID fd = open_log(PSP_O_APPEND);
        if (fd >= 0) {
            sceIoWrite(fd, text, len);
            sceIoClose(fd);
        }
        rt_io_end();
    }
}

/* Writes out everything in the buffer, on the calling thread. */
void rt_log_flush(void) {
    static char chunk[LOG_CHUNK];
    if (sBufferLock < 0) {
        return;
    }
    sceKernelWaitSema(sWriteLock, 1, NULL);
    for (;;) {
        sceKernelWaitSema(sBufferLock, 1, NULL);
        uint32_t len = sHead - sTail;
        if (len > LOG_CHUNK) {
            len = LOG_CHUNK;
        }
        for (uint32_t i = 0; i < len; i++) {
            chunk[i] = sBuffer[(sTail + i) & (LOG_BUFFER - 1)];
        }
        sceKernelSignalSema(sBufferLock, 1);
        if (len == 0) {
            break;
        }
        write_out(chunk, (int)len);
        /* only now is the room free: a line in the buffer is never lost */
        sceKernelWaitSema(sBufferLock, 1, NULL);
        sTail += len;
        sceKernelSignalSema(sBufferLock, 1);
    }
    sceKernelSignalSema(sWriteLock, 1);
}

static int writer_thread(SceSize args, void* argp) {
    for (;;) {
        sceKernelWaitSema(sPending, 1, NULL);
        rt_log_flush();
    }
    return 0;
}

void rt_log_init(void) {
    sSync = rt_data_file_exists("sync_log.txt");
    if (!sSync && !rt_data_file_exists("log.txt")) {
        return; /* no log: rt_log finds no buffer to put a line in */
    }
    sBufferLock = sceKernelCreateSema("rt_log", 0, 1, 1, NULL);
    sWriteLock = sceKernelCreateSema("rt_log_write", 0, 1, 1, NULL);
    sPending = sceKernelCreateSema("rt_log_pending", 0, 0, 1, NULL);
    SceUID fd = open_log(PSP_O_TRUNC);
    if (fd >= 0) {
        sceIoClose(fd);
        sLogOk = true;
    }
    /* Below the game and the renderer: it needs next to no CPU, only to wait for the stick. */
    SceUID thid = sceKernelCreateThread("rt_log", writer_thread, RT_GAME_THREAD_PRIORITY + 2, 16 * 1024,
                                        PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}

static void put_line(const char* line, int len) {
    for (;;) {
        sceKernelWaitSema(sBufferLock, 1, NULL);
        bool room = LOG_BUFFER - (sHead - sTail) >= (uint32_t)len;
        if (room) {
            for (int i = 0; i < len; i++) {
                sBuffer[(sHead + i) & (LOG_BUFFER - 1)] = line[i];
            }
            sHead += len;
        }
        sceKernelSignalSema(sBufferLock, 1);
        if (room && sSync) {
            rt_log_flush(); /* sync_log.txt: on the stick before anything else happens */
            return;
        }
        sceKernelSignalSema(sPending, 1);
        if (room) {
            return;
        }
        /* A flood (a draw trace): wait for the writer rather than lose lines. */
        sceKernelDelayThread(2000);
    }
}

/* One line, prefixed with the time since boot in seconds. */
static void log_va(const char* fmt, va_list args) {
    if (sBufferLock < 0) {
        return; /* no log (or not yet: before rt_log_init) */
    }
    char line[512];
    uint64_t now = sceKernelGetSystemTimeWide();
    int prefix = snprintf(line, sizeof(line), "[%6u.%03u] ", (unsigned)(now / 1000000), (unsigned)((now / 1000) % 1000));
    int len = vsnprintf(line + prefix, sizeof(line) - prefix - 2, fmt, args);
    if (len < 0) {
        return;
    }
    len += prefix;
    if (len > (int)sizeof(line) - 2) {
        len = sizeof(line) - 2;
    }
    if (line[len - 1] != '\n') {
        line[len++] = '\n';
    }
    line[len] = '\0';
    put_line(line, len);
}

void rt_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_va(fmt, args);
    va_end(args);
}

void rt_fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (sBufferLock < 0) {
        /* no log: the reason still goes to afpsp.log, by itself */
        char line[256];
        int len = vsnprintf(line, sizeof(line) - 1, fmt, args);
        SceUID fd = open_log(PSP_O_TRUNC);
        if (fd >= 0) {
            if (len > 0) {
                len = len < (int)sizeof(line) - 1 ? len : (int)sizeof(line) - 2;
                line[len++] = '\n';
                sceIoWrite(fd, line, len);
            }
            sceIoClose(fd);
        }
    } else {
        log_va(fmt, args);
    }
    va_end(args);
    rt_log("FATAL: stopping");
    rt_log_flush();
    sceKernelSleepThread();
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}
