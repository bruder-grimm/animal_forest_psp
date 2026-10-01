/*
 * data.c -- the port's data files (text_en.bin, names_en.bin).
 *
 * The Makefile packs them into EBOOT.PBP as its DATA.PSAR section, so an
 * installed EBOOT always carries the English text it was built with and no
 * loose file can be forgotten or left over from an older build. A PRX started
 * on its own (PSPLink) has no EBOOT: there the files next to it are used.
 *
 * The section (tools/make_psar.py): "AFDT", a count, then per file a 24-byte
 * name, its offset from the section's start and its size.
 */
#include <pspiofilemgr.h>
#include <string.h>

#include "rt.h"

typedef struct {
    char name[24];
    uint32_t offset, size;
} PackEntry;

static bool find_packed(SceUID fd, const char* name, uint32_t* base, uint32_t* size) {
    uint32_t pbp[10]; /* "\0PBP", version, eight section offsets: the last is DATA.PSAR */
    uint32_t top[2];
    if (sceIoRead(fd, pbp, sizeof(pbp)) != sizeof(pbp) || memcmp(pbp, "\0PBP", 4) != 0) {
        return false;
    }
    uint32_t psar = pbp[9];
    if (sceIoLseek32(fd, (int)psar, PSP_SEEK_SET) != (int)psar || sceIoRead(fd, top, sizeof(top)) != sizeof(top) ||
        memcmp(top, "AFDT", 4) != 0) {
        return false;
    }
    for (uint32_t i = 0; i < top[1] && i < 16; i++) {
        PackEntry e;
        if (sceIoRead(fd, &e, sizeof(e)) != sizeof(e)) {
            return false;
        }
        if (strncmp(e.name, name, sizeof(e.name)) == 0) {
            *base = psar + e.offset;
            *size = e.size;
            return true;
        }
    }
    return false;
}

int rt_data_open(const char* name, char* path, size_t path_size, uint32_t* base, uint32_t* size) {
    SceUID fd = sceIoOpen(rt_data_path("EBOOT.PBP", path, path_size), PSP_O_RDONLY, 0);
    if (fd >= 0) {
        if (find_packed(fd, name, base, size) && sceIoLseek32(fd, (int)*base, PSP_SEEK_SET) == (int)*base) {
            return fd;
        }
        sceIoClose(fd);
    }
    fd = sceIoOpen(rt_data_path(name, path, path_size), PSP_O_RDONLY, 0);
    if (fd >= 0) {
        *base = 0;
        *size = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_END);
        sceIoLseek32(fd, 0, PSP_SEEK_SET);
    }
    return fd;
}
