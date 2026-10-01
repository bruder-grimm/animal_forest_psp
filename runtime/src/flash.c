/*
 * flash.c -- FlashRAM (osFlash*), backed by flash.bin next to the EBOOT.
 *
 * The 128KB image is kept in memory and written back once writes have been
 * quiet for a moment (rt_save_tick), so a multi-page save becomes a single
 * file write. The asynchronous N64 API completes every request at once.
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
static uint8_t sWriteBuffer[FLASH_PAGE_BYTES];
static bool sDirty = false;
static uint32_t sQuietTicks = 0;

static const char* flash_path(char* out, size_t size) {
    return rt_data_path("flash.bin", out, size);
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
}

static void flush(void) {
    char path[256];
    char tmp_path[256];
    flash_path(path, sizeof(path));
    rt_data_path("flash.tmp", tmp_path, sizeof(tmp_path));

    /* Write to a temporary file and rename, so a power loss can't corrupt the save. */
    rt_io_begin();
    SceUID fd = sceIoOpen(tmp_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int written = -1;
    if (fd >= 0) {
        written = sceIoWrite(fd, sImage, sizeof(sImage));
        sceIoClose(fd);
        if (written == (int)sizeof(sImage)) {
            sceIoRemove(path);
            sceIoRename(tmp_path, path);
        }
    }
    rt_io_end();
    if (fd < 0) {
        rt_log("flash: cannot open %s (%08X)", tmp_path, (unsigned)fd);
        return;
    }
    if (written != (int)sizeof(sImage)) {
        rt_log("flash: short write (%d)", written);
        return;
    }
    sDirty = false;
    rt_log("flash: saved");
}

void rt_save_tick(void) {
    /* Runs every half second: write once a whole interval has passed without writes. */
    if (sDirty && ++sQuietTicks >= 2) {
        flush();
    }
}

void rt_save_flush(void) {
    if (sDirty) {
        flush();
    }
}

static void mark_dirty(void) {
    sDirty = true;
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

void __osFlashGetAddr(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = ctx->r4 * FLASH_PAGE_BYTES;
}

/*
 * flash_test.txt next to the EBOOT: once the game has started its threads, run
 * its own sFRm_Init and sFRm_WriteSync three times, as a save does. Every write
 * re-creates the flash thread on the same OSThread after osDestroyThread on the
 * finished one (see osCreateThread in sched.c). Pages 768-1023 are written with
 * whatever RDRAM holds; don't keep the flash.bin from such a run.
 */
void rt_debug_flash_test(recomp_context* ctx) {
    static const uint32_t pages[] = { 768, 896, 768 };
    recomp_context saved = *ctx;
    rt_log("flash test: initialising");
    get_function((int32_t)0x800CDB10)(g_rdram, ctx); /* sFRm_Init */
    for (unsigned i = 0; i < RT_COUNT(pages); i++) {
        ctx->r4 = 0x80100000;
        ctx->r5 = pages[i];
        ctx->r6 = 128;
        get_function((int32_t)0x800CE0E8)(g_rdram, ctx); /* sFRm_WriteSync */
        rt_log("flash test: write %u (page %u) done", i + 1, pages[i]);
    }
    *ctx = saved;
    rt_log("flash test: passed");
}
