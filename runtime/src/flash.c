/*
 * flash.c -- FlashRAM (osFlash*), backed by flash.bin next to the EBOOT.
 *
 * The 128KB image is kept in memory and written back once writes have been
 * quiet for a moment (rt_save_tick), so a multi-page save becomes a single
 * file write. The asynchronous N64 API completes every request at once.
 *
 * The file is written by a thread of its own, from a copy of the image: the
 * write takes 0.5-0.75 s on a memory stick, and done on the VI thread it held
 * back the retraces for as long -- picture and sound stopped a second after
 * every save, and after loading one, which marks the save as in use.
 */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <string.h>

#include "rt.h"

#define FLASH_PAGE_BYTES 128
#define FLASH_NUM_PAGES 1024
#define FLASH_TOTAL_BYTES (FLASH_PAGE_BYTES * FLASH_NUM_PAGES)
#define FLASH_SECTOR_PAGES 128

/* Macronix type/maker, accepted by sFRm_Init (FLASH_VERSION_MX_A). */
#define FLASH_TYPE 0x11118001u
#define FLASH_MAKER 0x00C20001u

static uint8_t sImage[FLASH_TOTAL_BYTES];
static uint8_t sSnapshot[FLASH_TOTAL_BYTES]; /* the copy that goes to the file */
static uint8_t sWriteBuffer[FLASH_PAGE_BYTES];
static uint32_t sChanges = 0;    /* counts the writes to sImage */
static uint32_t sSnapshotOf = 0; /* sChanges when sSnapshot was taken */
static uint32_t sSaved = 0;      /* sChanges of the snapshot in the file */
static uint32_t sQuietTicks = 0;
static SceUID sFileLock = -1;    /* guards sSnapshot and the file */
static SceUID sPending = -1;     /* wakes the writer */
static SceUID sWriter = -1;

static const char* flash_path(char* out, size_t size) {
    return rt_data_path("flash.bin", out, size);
}

/* Writes sSnapshot to the file. The caller holds sFileLock. */
static void write_snapshot(void) {
    char path[256];
    char tmp_path[256];
    flash_path(path, sizeof(path));
    rt_data_path("flash.tmp", tmp_path, sizeof(tmp_path));

    /* Write to a temporary file and rename, so a power loss can't corrupt the save. */
    rt_io_begin();
    SceUID fd = sceIoOpen(tmp_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int written = -1;
    if (fd >= 0) {
        written = sceIoWrite(fd, sSnapshot, sizeof(sSnapshot));
        sceIoClose(fd);
        if (written == (int)sizeof(sSnapshot)) {
            sceIoRemove(path);
            sceIoRename(tmp_path, path);
        }
    }
    rt_io_end();
    if (fd < 0) {
        rt_log("flash: cannot open %s (%08X)", tmp_path, (unsigned)fd);
        return;
    }
    if (written != (int)sizeof(sSnapshot)) {
        rt_log("flash: short write (%d)", written);
        return;
    }
    sSaved = sSnapshotOf;
    rt_log("flash: saved");
}

static void take_snapshot(void) {
    sSnapshotOf = sChanges;
    memcpy(sSnapshot, sImage, sizeof(sSnapshot));
}

static int writer_thread(SceSize args, void* argp) {
    for (;;) {
        sceKernelWaitSema(sPending, 1, NULL);
        sceKernelWaitSema(sFileLock, 1, NULL);
        if (sSnapshotOf != sSaved) {
            write_snapshot();
        }
        sceKernelSignalSema(sFileLock, 1);
    }
    return 0;
}

void rt_save_init(void) {
    char path[256];
    memset(sImage, 0xFF, sizeof(sImage));
    SceUID fd = sceIoOpen(flash_path(path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int got = sceIoRead(fd, sImage, sizeof(sImage));
        sceIoClose(fd);
        rt_log("flash: loaded %d bytes from %s", got, path);
    } else {
        rt_log("flash: no save at %s, starting erased", path);
    }

    sFileLock = sceKernelCreateSema("rt_save", 0, 1, 1, NULL);
    sPending = sceKernelCreateSema("rt_save_pending", 0, 0, 1, NULL);
    /* Below the game and the renderer, like the log's writer: it only waits for the stick. */
    sWriter = sceKernelCreateThread("rt_save", writer_thread, RT_GAME_THREAD_PRIORITY + 2, 16 * 1024,
                                    PSP_THREAD_ATTR_USER, NULL);
    if (sWriter >= 0 && sceKernelStartThread(sWriter, 0, NULL) < 0) {
        sWriter = -1;
    }
}

void rt_save_tick(void) {
    /* Runs every half second, on the VI thread: once a whole interval has passed
     * without writes, copy the image -- no game thread runs meanwhile, so the
     * copy is of one moment -- and leave the file to the writer. If the writer
     * is still busy with the previous copy, the next tick tries again. */
    if (sChanges == sSaved || ++sQuietTicks < 2 || sceKernelPollSema(sFileLock, 1) < 0) {
        return;
    }
    take_snapshot();
    if (sWriter < 0) {
        write_snapshot(); /* no writer thread: here, as a last resort */
    }
    sceKernelSignalSema(sFileLock, 1);
    sceKernelSignalSema(sPending, 1);
}

/* On exit: waits for a write in progress, then writes what is newer than the file. */
void rt_save_flush(void) {
    sceKernelWaitSema(sFileLock, 1, NULL);
    if (sChanges != sSaved) {
        take_snapshot();
        write_snapshot();
    }
    sceKernelSignalSema(sFileLock, 1);
}

static void mark_dirty(void) {
    sChanges++;
    sQuietTicks = 0;
}

/* The request's completion message; queued if the queue is full right now. */
static void complete(uint32_t mb, uint32_t mq) {
    if (mq != 0 && !rt_send_now(mq, mb, false)) {
        rt_post_message(mq, mb, false, true);
    }
}

void osFlashInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t h = RT_FLASH_HANDLE;
    wr_w32(h + 0x00, 0);
    wr_u8(h + 0x04, 8); /* DEVICE_TYPE_FLASH */
    wr_w32(h + 0x0C, 0xA8000000);
    ctx->r2 = h;
}

void osFlashReadStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr_u8(ctx->r4, 0);
}

void osFlashReadId_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr_w32(ctx->r4, FLASH_TYPE);
    wr_w32(ctx->r5, FLASH_MAKER);
}

RT_STUB(osFlashClearStatus_recomp)

void osFlashAllErase_recomp(uint8_t* rdram, recomp_context* ctx) {
    memset(sImage, 0xFF, sizeof(sImage));
    mark_dirty();
    ctx->r2 = 0;
}

void osFlashSectorErase_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t page = ctx->r4;
    uint32_t sector = (page / FLASH_SECTOR_PAGES) * FLASH_SECTOR_PAGES;
    if (sector >= FLASH_NUM_PAGES) {
        ctx->r2 = (gpr)-1;
        return;
    }
    uint32_t count = FLASH_SECTOR_PAGES;
    if (sector + count > FLASH_NUM_PAGES) {
        count = FLASH_NUM_PAGES - sector;
    }
    memset(&sImage[sector * FLASH_PAGE_BYTES], 0xFF, count * FLASH_PAGE_BYTES);
    mark_dirty();
    ctx->r2 = 0;
}

/* s32 osFlashWriteBuffer(OSIoMesg* mb, s32 priority, void* dramAddr, OSMesgQueue* mq) */
void osFlashWriteBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_copy_from_rdram(ctx->r6, sWriteBuffer, FLASH_PAGE_BYTES);
    complete(ctx->r4, ctx->r7);
    ctx->r2 = 0;
}

/* s32 osFlashWriteArray(u32 page_num) */
void osFlashWriteArray_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t page = ctx->r4;
    if (page >= FLASH_NUM_PAGES) {
        ctx->r2 = (gpr)-1;
        return;
    }
    memcpy(&sImage[page * FLASH_PAGE_BYTES], sWriteBuffer, FLASH_PAGE_BYTES);
    mark_dirty();
    ctx->r2 = 0;
}

/* s32 osFlashReadArray(OSIoMesg* mb, s32 priority, u32 page_num, void* dramAddr, u32 n_pages, OSMesgQueue* mq) */
void osFlashReadArray_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mb = ctx->r4;
    uint32_t page = ctx->r6;
    uint32_t addr = ctx->r7;
    uint32_t n_pages = rt_stack_arg(ctx, 4);
    uint32_t mq = rt_stack_arg(ctx, 5);

    if (page < FLASH_NUM_PAGES) {
        uint32_t count = n_pages;
        if (page + count > FLASH_NUM_PAGES) {
            count = FLASH_NUM_PAGES - page;
        }
        rt_copy_to_rdram(addr, &sImage[page * FLASH_PAGE_BYTES], count * FLASH_PAGE_BYTES);
    }
    complete(mb, mq);
    ctx->r2 = 0;
}

