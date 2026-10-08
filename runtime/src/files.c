/*
 * files.c -- the files next to the EBOOT: their paths, reading and writing
 * them, the debug switches among them, and the port's data packed into the
 * EBOOT itself.
 *
 * Memory stick access while the game runs follows two rules that standby
 * brought (main.c): every access holds the power lock (rt_io_begin/end), and
 * a file kept open across a standby has to be opened again (rt_read_at).
 */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

/* Where the EBOOT (or a PRX started on its own) is; set from argv[0]. */
static char sBaseDir[192] = "ms0:/PSP/GAME/AFPSP";

void rt_files_init(const char* argv0) {
    const char* slash = argv0 != NULL ? strrchr(argv0, '/') : NULL;
    if (slash == NULL) {
        return;
    }
    size_t len = (size_t)(slash - argv0);
    if (len >= sizeof(sBaseDir)) {
        return;
    }
    memcpy(sBaseDir, argv0, len);
    sBaseDir[len] = '\0';
}

const char* rt_data_path(const char* name, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", sBaseDir, name);
    return out;
}

/* ---- the memory stick and standby -------------------------------------- */

/*
 * Brackets every memory stick access made while the game runs. Going into
 * standby waits while the lock is held, and a thread that asks for it during
 * a standby waits until the PSP is back: I/O that overlapped the suspend left
 * the stick driver stuck after the resume.
 */
void rt_io_begin(void) {
    scePowerLock(0);
}

void rt_io_end(void) {
    scePowerUnlock(0);
}

/*
 * Reads len bytes at offset from *fd, which was opened read-only from path.
 * Standby closes every open file on the memory stick, and for a while after
 * resuming the stick isn't there at all: a failed read reopens the file and
 * tries again, for up to 10 s. Returns what the last read returned.
 */
int rt_read_at(int* fd, const char* path, uint32_t offset, void* buf, int len) {
    for (int attempt = 0;; attempt++) {
        int got = -1;
        rt_io_begin();
        if (*fd >= 0 && sceIoLseek32(*fd, (int)offset, PSP_SEEK_SET) == (int)offset) {
            got = sceIoRead(*fd, buf, (SceSize)len);
        }
        rt_io_end();
        if (got >= len) {
            if (attempt > 0) {
                rt_log("%s: reopened after %d attempts", path, attempt);
            }
            return got;
        }
        if (attempt == 100) {
            rt_log("%s: read of %d bytes at %08X failed (%08X)", path, len, (unsigned)offset, (unsigned)got);
            return got;
        }
        rt_io_begin();
        if (*fd >= 0) {
            sceIoClose(*fd);
        }
        rt_io_end();
        sceKernelDelayThread(100000);
        rt_io_begin();
        *fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        rt_io_end();
        if (attempt == 0 || attempt == 9 || attempt == 49) {
            rt_log("%s: read at %08X failed (%08X), reopened: %08X", path, (unsigned)offset, (unsigned)got,
                   (unsigned)*fd);
        }
    }
}

/* ---- reading and writing ----------------------------------------------- */

bool rt_data_file_exists(const char* name) {
    char path[256];
    SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    sceIoClose(fd);
    return true;
}

int rt_data_read(const char* name, void* buf, int size) {
    char path[256];
    rt_io_begin();
    SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_RDONLY, 0);
    int got = -1;
    if (fd >= 0) {
        got = sceIoRead(fd, buf, (SceSize)size);
        sceIoClose(fd);
    }
    rt_io_end();
    return got;
}

int rt_data_read_text(const char* name, char* text, int size) {
    int len = rt_data_read(name, text, size - 1);
    if (len < 0) {
        return -1;
    }
    text[len] = '\0';
    return len;
}

int rt_data_create(const char* name, char* path, size_t path_size) {
    return sceIoOpen(rt_data_path(name, path, path_size), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
}

bool rt_data_write(const char* name, const void* data, uint32_t size) {
    char path[256];
    rt_io_begin();
    SceUID fd = rt_data_create(name, path, sizeof(path));
    bool ok = fd >= 0 && sceIoWrite(fd, data, size) == (int)size;
    if (fd >= 0) {
        sceIoClose(fd);
    }
    rt_io_end();
    return ok;
}

bool rt_data_rename(const char* from, const char* to) {
    char from_path[256], to_path[256];
    rt_data_path(from, from_path, sizeof(from_path));
    rt_data_path(to, to_path, sizeof(to_path));
    sceIoRemove(to_path);
    return sceIoRename(from_path, to_path) >= 0;
}

/* ---- debug switches with numbers --------------------------------------- */

/* The numbers may be separated by anything that isn't a digit. */
int rt_load_number_list(const char* file, uint32_t* out, int max) {
    char text[512];
    if (rt_data_read_text(file, text, sizeof(text)) <= 0) {
        return 0;
    }
    int count = 0;
    for (char* p = text; *p != '\0' && count < max;) {
        if (*p < '0' || *p > '9') {
            p++;
            continue;
        }
        out[count++] = (uint32_t)strtoul(p, &p, 10);
    }
    return count;
}

bool rt_numbers_have(RtNumbers* list, uint32_t value) {
    if (list->count < 0) {
        list->count = rt_load_number_list(list->file, list->values, (int)RT_COUNT(list->values));
    }
    for (int i = 0; i < list->count; i++) {
        if (list->values[i] == value) {
            return true;
        }
    }
    return false;
}

/* ---- data packed into the EBOOT ----------------------------------------- */

/*
 * The port's data files (text_en.bin, names_en.bin): the Makefile packs them
 * into EBOOT.PBP as its DATA.PSAR section, so an installed EBOOT always
 * carries the English text it was built with and no loose file can be
 * forgotten or left over from an older build. A PRX started on its own
 * (PSPLink) has no EBOOT: there the files next to it are used.
 *
 * The section (tools/make_psar.py): "AFDT", a count, then per file a 24-byte
 * name, its offset from the section's start and its size.
 */
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
