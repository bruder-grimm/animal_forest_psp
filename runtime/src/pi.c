/*
 * pi.c -- cartridge ROM access (PI DMA) and RDRAM copy helpers.
 *
 * The ROM is read from the memory stick on demand. Any common byte order is
 * accepted (.z64 big-endian, .v64 byte-swapped, .n64 little-endian) and
 * normalised to big-endian as it is read.
 */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define ROM_PHYS_BASE 0x10000000u

/*
 * ROM cache: blocks kept in RDRAM word order (host-endian 32-bit words), so
 * DMA copies into RDRAM are plain memcpy. Audio samples are streamed from
 * all over the ROM, hence many small blocks with LRU eviction. A block is
 * found through sSlotOf (ROM block -> cache slot); the per-slot bookkeeping
 * is kept apart from the 32 KB blocks, so looking a block up or choosing one
 * to evict touches a few cache lines rather than one per block.
 */
#define ROM_BLOCK_WORDS 8192
#define ROM_BLOCK_BYTES (ROM_BLOCK_WORDS * 4)
#define ROM_CACHE_MIN_BLOCKS 32   /* 1 MB, what a PSP-1000 can spare */
/* Left for the texture cache, the display list and decode scratch. */
#define ROM_CACHE_RESERVE (14u * 1024 * 1024)
#define NO_BLOCK UINT32_MAX

typedef enum {
    ROM_Z64,
    ROM_V64,
    ROM_N64,
} RomOrder;

static SceUID sRomFd = -1;
static char sRomPath[256];
static uint32_t sRomSize = 0;
static RomOrder sRomOrder = ROM_Z64;
static uint32_t* sBlockWords;     /* sNumSlots blocks of ROM_BLOCK_WORDS */
static uint32_t* sSlotBlock;      /* per slot: the ROM block it holds, NO_BLOCK if none */
static uint32_t* sSlotLastUsed;   /* per slot: sUseCounter at its last use */
static int16_t* sSlotOf;          /* per ROM block: its slot, -1 if not cached */
static int sNumSlots;
static uint32_t sBlockUs;
static uint32_t sUseCounter = 0;
static SceUID sRomLock = -1;
static uint32_t sBlockLoads = 0;

/* ---- RDRAM copy helpers ------------------------------------------------- */

/* The unaligned ends go a byte at a time, whole words with one byte swap each. */
void rt_copy_to_rdram(uint32_t addr, const uint8_t* src, uint32_t size) {
    uint8_t* rdram = g_rdram;
    for (; size > 0 && (addr & 3) != 0; size--) {
        MEM_BU(0, addr++) = *src++;
    }
    for (; size >= 4; size -= 4, addr += 4, src += 4) {
        uint32_t w;
        memcpy(&w, src, 4);
        MEM_WU(0, addr) = __builtin_bswap32(w);
    }
    for (; size > 0; size--) {
        MEM_BU(0, addr++) = *src++;
    }
}

void rt_copy_from_rdram(uint32_t addr, uint8_t* dst, uint32_t size) {
    uint8_t* rdram = g_rdram;
    for (; size > 0 && (addr & 3) != 0; size--) {
        *dst++ = MEM_BU(0, addr++);
    }
    for (; size >= 4; size -= 4, addr += 4, dst += 4) {
        uint32_t w = __builtin_bswap32(MEM_WU(0, addr));
        memcpy(dst, &w, 4);
    }
    for (; size > 0; size--) {
        *dst++ = MEM_BU(0, addr++);
    }
}

void rt_fill_rdram(uint32_t addr, uint8_t value, uint32_t size) {
    uint8_t* rdram = g_rdram;
    for (uint32_t i = 0; i < size; i++) {
        MEM_BU(i, addr) = value;
    }
}

/* ---- ROM file ----------------------------------------------------------- */

static void normalise(uint8_t* buf, uint32_t len) {
    if (sRomOrder == ROM_V64) {
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            uint8_t t = buf[i];
            buf[i] = buf[i + 1];
            buf[i + 1] = t;
        }
    } else if (sRomOrder == ROM_N64) {
        for (uint32_t i = 0; i + 3 < len; i += 4) {
            uint8_t t0 = buf[i];
            uint8_t t1 = buf[i + 1];
            buf[i] = buf[i + 3];
            buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1;
            buf[i + 3] = t0;
        }
    }
}

bool rt_rom_open(void) {
    static const char* names[] = { "baserom.z64", "baserom.n64", "baserom.v64", "rom.z64", "rom.n64", "rom.v64" };
    char path[256];

    sRomLock = sceKernelCreateSema("rt_rom", 0, 1, 1, NULL);

    /* rom_path.txt names the ROM when it is not next to the EBOOT -- reading it
     * over a slow link (PSPLink's host0:) stalls the game when it streams. */
    SceUID fd = sceIoOpen(rt_data_path("rom_path.txt", path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd >= 0) {
        char text[256];
        int len = sceIoRead(fd, text, sizeof(text) - 1);
        sceIoClose(fd);
        if (len > 0) {
            text[len] = '\0';
            for (int i = 0; i < len; i++) {
                if (text[i] == '\r' || text[i] == '\n') {
                    text[i] = '\0';
                    break;
                }
            }
            sRomFd = sceIoOpen(text, PSP_O_RDONLY, 0);
            if (sRomFd >= 0) {
                snprintf(sRomPath, sizeof(sRomPath), "%s", text);
                rt_log("ROM: %s (from rom_path.txt)", text);
            } else {
                rt_log("ROM: %s from rom_path.txt could not be opened", text);
            }
        }
    }

    for (size_t i = 0; sRomFd < 0 && i < RT_COUNT(names); i++) {
        rt_data_path(names[i], path, sizeof(path));
        sRomFd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (sRomFd >= 0) {
            snprintf(sRomPath, sizeof(sRomPath), "%s", path);
            rt_log("ROM: %s", path);
            break;
        }
    }
    if (sRomFd < 0) {
        return false;
    }

    sRomSize = (uint32_t)sceIoLseek(sRomFd, 0, PSP_SEEK_END);
    sceIoLseek(sRomFd, 0, PSP_SEEK_SET);

    /*
     * Cache as much of the ROM as the machine can spare: the game streams
     * audio samples and area data from all over it, and a block that has to be
     * read again stalls whichever thread asked for it. On a 64 MB PSP the whole
     * ROM fits, which removes the stalls entirely.
     */
    uint32_t free_mem = rt_free_memory();
    uint32_t budget = free_mem > ROM_CACHE_RESERVE ? free_mem - ROM_CACHE_RESERVE : 0;
    int blocks = (int)(budget / ROM_BLOCK_BYTES);
    int needed = (int)((sRomSize + ROM_BLOCK_BYTES - 1) / ROM_BLOCK_BYTES);
    if (blocks > needed) {
        blocks = needed;
    }
    while (blocks >= ROM_CACHE_MIN_BLOCKS && sBlockWords == NULL) {
        sBlockWords = malloc((size_t)blocks * ROM_BLOCK_BYTES);
        if (sBlockWords == NULL) {
            blocks /= 2;
        }
    }
    if (sBlockWords == NULL) {
        blocks = ROM_CACHE_MIN_BLOCKS;
        sBlockWords = malloc((size_t)blocks * ROM_BLOCK_BYTES);
    }
    sSlotBlock = malloc((size_t)blocks * sizeof(*sSlotBlock));
    sSlotLastUsed = calloc((size_t)blocks, sizeof(*sSlotLastUsed));
    sSlotOf = malloc((size_t)needed * sizeof(*sSlotOf));
    if (sBlockWords == NULL || sSlotBlock == NULL || sSlotLastUsed == NULL || sSlotOf == NULL) {
        return false;
    }
    sNumSlots = blocks;
    for (int i = 0; i < blocks; i++) {
        sSlotBlock[i] = NO_BLOCK;
    }
    memset(sSlotOf, 0xFF, (size_t)needed * sizeof(*sSlotOf));
    rt_log("ROM cache: %d blocks (%u KB%s)", blocks, (unsigned)((uint32_t)blocks * ROM_BLOCK_BYTES / 1024),
           blocks == needed ? ", the whole ROM" : "");

    uint8_t magic[4];
    sceIoRead(sRomFd, magic, 4);
    if (magic[0] == 0x80 && magic[1] == 0x37) {
        sRomOrder = ROM_Z64;
    } else if (magic[0] == 0x37 && magic[1] == 0x80) {
        sRomOrder = ROM_V64;
    } else if (magic[0] == 0x40 && magic[1] == 0x12) {
        sRomOrder = ROM_N64;
    } else {
        rt_log("ROM: unrecognised header %02X%02X%02X%02X", magic[0], magic[1], magic[2], magic[3]);
        return false;
    }
    rt_log("ROM: %u bytes, byte order %s", (unsigned)sRomSize,
           sRomOrder == ROM_Z64 ? "z64" : (sRomOrder == ROM_V64 ? "v64" : "n64"));
    return true;
}

/* Reads ROM block `index` into words (host-order words, as RDRAM holds them). */
static void read_block(uint32_t* words, uint32_t index) {
    uint8_t* bytes = (uint8_t*)words;
    uint32_t offset = index * ROM_BLOCK_BYTES;
    uint32_t t0 = sceKernelGetSystemTimeLow();
    int want = offset < sRomSize ? (int)(sRomSize - offset) : 0;
    if (want > ROM_BLOCK_BYTES) {
        want = ROM_BLOCK_BYTES;
    }
    int got = want > 0 ? rt_read_at(&sRomFd, sRomPath, offset, bytes, want) : 0;
    if (got < 0) {
        got = 0;
    }
    if (got < ROM_BLOCK_BYTES) {
        memset(bytes + got, 0xFF, ROM_BLOCK_BYTES - got);
    }
    /* file order -> big-endian bytes -> host-order words */
    normalise(bytes, ROM_BLOCK_BYTES);
    for (uint32_t i = 0; i < ROM_BLOCK_WORDS; i++) {
        uint32_t w;
        memcpy(&w, bytes + i * 4, 4);
        words[i] = __builtin_bswap32(w);
    }
    sBlockLoads++;
    sBlockUs += sceKernelGetSystemTimeLow() - t0;
}

/* The words of ROM block `index`, read into the least recently used slot if
 * they aren't cached. The caller holds sRomLock and has checked that the
 * block lies inside the ROM. */
static const uint32_t* get_block(uint32_t index) {
    int slot = sSlotOf[index];
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < sNumSlots; i++) {
            if (sSlotLastUsed[i] < sSlotLastUsed[slot]) {
                slot = i;
            }
        }
        if (sSlotBlock[slot] != NO_BLOCK) {
            sSlotOf[sSlotBlock[slot]] = -1;
        }
        read_block(&sBlockWords[(uint32_t)slot * ROM_BLOCK_WORDS], index);
        sSlotBlock[slot] = index;
        sSlotOf[index] = (int16_t)slot;
    }
    sSlotLastUsed[slot] = ++sUseCounter;
    return &sBlockWords[(uint32_t)slot * ROM_BLOCK_WORDS];
}

static inline uint8_t word_byte(uint32_t w, uint32_t offset) {
    return (uint8_t)(w >> (24 - 8 * (offset & 3)));
}

void rt_rom_read(uint32_t offset, uint8_t* dst, uint32_t size) {
    sceKernelWaitSema(sRomLock, 1, NULL);
    while (size > 0) {
        if (offset >= sRomSize) {
            memset(dst, 0xFF, size);
            break;
        }
        const uint32_t* words = get_block(offset / ROM_BLOCK_BYTES);
        uint32_t in_block = offset % ROM_BLOCK_BYTES;
        uint32_t n = ROM_BLOCK_BYTES - in_block;
        if (n > size) {
            n = size;
        }
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = word_byte(words[(in_block + i) >> 2], in_block + i);
        }
        dst += n;
        offset += n;
        size -= n;
    }
    sceKernelSignalSema(sRomLock, 1);
}

void rt_rom_read_to_rdram(uint32_t rom_offset, uint32_t addr, uint32_t size) {
    uint8_t* rdram = g_rdram;
    sceKernelWaitSema(sRomLock, 1, NULL);
    while (size > 0) {
        if (rom_offset >= sRomSize) {
            break;
        }
        const uint32_t* words = get_block(rom_offset / ROM_BLOCK_BYTES);
        uint32_t in_block = rom_offset % ROM_BLOCK_BYTES;
        uint32_t n = ROM_BLOCK_BYTES - in_block;
        if (n > size) {
            n = size;
        }
        uint32_t i = 0;
        if (((in_block ^ addr) & 3) == 0) {
            /* aligned: whole words at once */
            for (; i < n && ((in_block + i) & 3) != 0; i++) {
                MEM_BU(i, addr) = word_byte(words[(in_block + i) >> 2], in_block + i);
            }
            uint32_t count = (n - i) >> 2;
            if (count > 0) {
                memcpy(rdram + MEM_OFFSET(i, addr), &words[(in_block + i) >> 2], count * 4);
                i += count * 4;
            }
        }
        for (; i < n; i++) {
            MEM_BU(i, addr) = word_byte(words[(in_block + i) >> 2], in_block + i);
        }
        rom_offset += n;
        addr += n;
        size -= n;
    }
    sceKernelSignalSema(sRomLock, 1);
}

/* Physical address -> ROM offset, or UINT32_MAX if not cartridge ROM. */
static uint32_t phys_to_rom(uint32_t phys) {
    phys &= 0x1FFFFFFF;
    if (phys >= ROM_PHYS_BASE && phys < 0x1FC00000) {
        return phys - ROM_PHYS_BASE;
    }
    return UINT32_MAX;
}

static void do_dma(uint32_t phys, uint32_t ram, uint32_t size, uint32_t direction) {
    uint32_t rom_offset = phys_to_rom(phys);
    if (direction != 0) {
        RT_LOG_ONCE("PI DMA write to %08X ignored", phys);
        return;
    }
    if (rom_offset == UINT32_MAX) {
        rt_log("PI DMA read from non-ROM address %08X", phys);
        rt_fill_rdram(ram, 0, size);
        return;
    }
    rt_rom_read_to_rdram(rom_offset, ram, size);
}

static uint32_t read_io_word(uint32_t phys) {
    uint8_t b[4];
    uint32_t rom_offset = phys_to_rom(phys);
    if (rom_offset == UINT32_MAX) {
        return 0;
    }
    rt_rom_read(rom_offset, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

/* ---- asynchronous DMA --------------------------------------------------- */

/* Cartridge reads complete asynchronously, like the PI hardware: the request
 * goes to an I/O thread and the completion message is posted when the data
 * is in RDRAM, so game threads keep running while the file is read. */
#define DMA_QUEUE_SIZE 64

typedef struct {
    uint32_t phys, ram, size, direction, mq, mb;
    uint32_t queued_us;
} DmaRequest;

static uint32_t sDmaCount, sDmaBytes, sDmaUs, sDmaMaxUs;

static DmaRequest sDmaQueue[DMA_QUEUE_SIZE];
static unsigned sDmaHead = 0, sDmaTail = 0;
static SceUID sDmaLock = -1;
static SceUID sDmaSignal = -1;

static int dma_thread(SceSize args, void* argp) {
    for (;;) {
        sceKernelWaitSema(sDmaSignal, 1, NULL);
        sceKernelWaitSema(sDmaLock, 1, NULL);
        DmaRequest r = sDmaQueue[sDmaHead];
        sDmaHead = (sDmaHead + 1) % DMA_QUEUE_SIZE;
        sceKernelSignalSema(sDmaLock, 1);
        do_dma(r.phys, r.ram, r.size, r.direction);
        rt_post_message(r.mq, r.mb, false, true);
        uint32_t took = sceKernelGetSystemTimeLow() - r.queued_us;
        sDmaCount++;
        sDmaBytes += r.size;
        sDmaUs += took;
        if (took > sDmaMaxUs) {
            sDmaMaxUs = took;
        }
    }
    return 0;
}

static void start_dma(uint32_t phys, uint32_t ram, uint32_t size, uint32_t direction, uint32_t mq, uint32_t mb) {
    if (sDmaSignal < 0) {
        sDmaLock = sceKernelCreateSema("rt_dma_lock", 0, 1, 1, NULL);
        sDmaSignal = sceKernelCreateSema("rt_dma", 0, 0, DMA_QUEUE_SIZE, NULL);
        SceUID thid = sceKernelCreateThread("rt_dma", dma_thread, 0x1C, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
        sceKernelStartThread(thid, 0, NULL);
    }
    sceKernelWaitSema(sDmaLock, 1, NULL);
    unsigned next = (sDmaTail + 1) % DMA_QUEUE_SIZE;
    if (next == sDmaHead) {
        /* queue full: do it now */
        sceKernelSignalSema(sDmaLock, 1);
        do_dma(phys, ram, size, direction);
        rt_post_message(mq, mb, false, true);
        return;
    }
    sDmaQueue[sDmaTail] = (DmaRequest){ phys, ram, size, direction, mq, mb, sceKernelGetSystemTimeLow() };
    sDmaTail = next;
    sceKernelSignalSema(sDmaLock, 1);
    sceKernelSignalSema(sDmaSignal, 1);
}

void rt_dma_report(char* buf, int size) {
    snprintf(buf, size, "dma %u reqs, %u KB, %u ms total, worst %u ms", (unsigned)sDmaCount, (unsigned)(sDmaBytes / 1024),
             (unsigned)(sDmaUs / 1000), (unsigned)(sDmaMaxUs / 1000));
    sDmaCount = sDmaBytes = sDmaUs = sDmaMaxUs = 0;
}

uint32_t rt_rom_block_us(void) {
    return sBlockUs;
}

uint32_t rt_rom_block_loads(void) {
    return sBlockLoads;
}

/* ---- libultra ----------------------------------------------------------- */

#define PIHANDLE_BASE 0x0C
#define IOMESG_RETQUEUE 0x04
#define IOMESG_DRAMADDR 0x08
#define IOMESG_DEVADDR 0x0C
#define IOMESG_SIZE 0x10

void osCartRomInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t h = RT_CART_HANDLE;
    wr_w32(h + 0x00, 0);
    wr_u8(h + 0x04, 0);    /* type: cartridge */
    wr_u8(h + 0x05, 0xFF); /* latency */
    wr_u8(h + 0x06, 0x0F); /* page size */
    wr_u8(h + 0x07, 0x03); /* release duration */
    wr_u8(h + 0x08, 0xFF); /* pulse */
    wr_u8(h + 0x09, 0);    /* domain */
    wr_w32(h + PIHANDLE_BASE, 0xB0000000);
    wr_w32(h + 0x10, 0);
    ctx->r2 = h;
}

RT_STUB(osCreatePiManager_recomp)

/* s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) */
void osEPiStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t handle = ctx->r4;
    uint32_t mb = ctx->r5;
    uint32_t direction = ctx->r6;
    uint32_t base = rd_w32(handle + PIHANDLE_BASE);
    uint32_t dev = rd_w32(mb + IOMESG_DEVADDR);
    uint32_t ram = rd_w32(mb + IOMESG_DRAMADDR);
    uint32_t size = rd_w32(mb + IOMESG_SIZE);
    uint32_t retq = rd_w32(mb + IOMESG_RETQUEUE);

    start_dma(base | dev, ram, size, direction, retq, mb);
    ctx->r2 = 0;
}

/* s32 osEPiReadIo(OSPiHandle* handle, u32 devAddr, u32* data) */
void osEPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t base = rd_w32(ctx->r4 + PIHANDLE_BASE);
    wr_w32(ctx->r6, read_io_word(base | ctx->r5));
    ctx->r2 = 0;
}

