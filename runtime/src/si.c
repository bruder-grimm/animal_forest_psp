/*
 * si.c -- serial interface: controllers, PIF commands and the cartridge RTC.
 *
 * The high-level controller API (osCont*) is answered directly from the PSP
 * pad. Raw PIF access (__osSiRawStartDma) is emulated at the joybus level;
 * Animal Forest uses it for the cartridge real-time clock on channel 4,
 * which is backed by the PSP's clock plus a user-settable offset.
 */
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

/* ---- pad ---------------------------------------------------------------- */

#define N64_A 0x8000
#define N64_B 0x4000
#define N64_Z 0x2000
#define N64_START 0x1000
#define N64_DU 0x0800
#define N64_DD 0x0400
#define N64_DL 0x0200
#define N64_DR 0x0100
#define N64_L 0x0020
#define N64_R 0x0010
#define N64_CU 0x0008
#define N64_CD 0x0004
#define N64_CL 0x0002
#define N64_CR 0x0001

#define CONT_NO_RESPONSE_ERROR 0x8

static uint16_t sButtons = 0;
static int8_t sStickX = 0;
static int8_t sStickY = 0;

/*
 * Scripted input for testing (input_script.txt next to the EBOOT), timed in
 * controller polls so runs replay the same way regardless of speed. Lines:
 *   <poll>[-<last>/<period>] [x<hold>] TOKEN...
 * TOKEN: A B Z L R START CU CD CL CR DU DD DL DR, X=<-80..80>, Y=<-80..80>.
 * Each activation lasts <hold> polls (default 3). '#' starts a comment.
 */
typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t period;
    uint32_t hold;
    uint16_t buttons;
    int8_t x;
    int8_t y;
    bool has_stick;
} ScriptEntry;

#define MAX_SCRIPT 512
static ScriptEntry sScript[MAX_SCRIPT];
static int sScriptLen = -1;
static uint32_t sPolls = 0;

static const struct {
    const char* name;
    uint16_t mask;
} kButtonNames[] = {
    { "A", N64_A }, { "B", N64_B }, { "Z", N64_Z }, { "L", N64_L }, { "R", N64_R }, { "START", N64_START },
    { "CU", N64_CU }, { "CD", N64_CD }, { "CL", N64_CL }, { "CR", N64_CR },
    { "DU", N64_DU }, { "DD", N64_DD }, { "DL", N64_DL }, { "DR", N64_DR },
};

static void load_script(void) {
    sScriptLen = 0;
    char path[256];
    SceUID fd = sceIoOpen(rt_data_path("input_script.txt", path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    static char text[32 * 1024];
    int len = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (len <= 0) {
        return;
    }
    text[len] = 0;
    char* line = text;
    while (line != NULL && *line && sScriptLen < MAX_SCRIPT) {
        char* next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = 0;
        }
        char* hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = 0;
        }
        ScriptEntry e = { 0, 0, 1, 3, 0, 0, 0, false };
        char* tok = strtok(line, " \t\r");
        if (tok != NULL) {
            char* end;
            e.first = (uint32_t)strtoul(tok, &end, 10);
            e.last = e.first;
            if (*end == '-') {
                e.last = (uint32_t)strtoul(end + 1, &end, 10);
                if (*end == '/') {
                    e.period = (uint32_t)strtoul(end + 1, &end, 10);
                }
            }
            if (e.period == 0) {
                e.period = 1;
            }
            while ((tok = strtok(NULL, " \t\r")) != NULL) {
                if (tok[0] == 'x' && tok[1] >= '0' && tok[1] <= '9') {
                    e.hold = (uint32_t)strtoul(tok + 1, NULL, 10);
                } else if (tok[0] == 'X' && tok[1] == '=') {
                    e.x = (int8_t)strtol(tok + 2, NULL, 10);
                    e.has_stick = true;
                } else if (tok[0] == 'Y' && tok[1] == '=') {
                    e.y = (int8_t)strtol(tok + 2, NULL, 10);
                    e.has_stick = true;
                } else {
                    for (size_t i = 0; i < RT_COUNT(kButtonNames); i++) {
                        if (strcmp(tok, kButtonNames[i].name) == 0) {
                            e.buttons |= kButtonNames[i].mask;
                        }
                    }
                }
            }
            sScript[sScriptLen++] = e;
        }
        line = next;
    }
    rt_log("input: %d scripted entries from %s", sScriptLen, path);
}

static void apply_script(uint16_t* buttons, int8_t* x, int8_t* y) {
    for (int i = 0; i < sScriptLen; i++) {
        const ScriptEntry* e = &sScript[i];
        if (sPolls < e->first || sPolls >= e->last + e->hold) {
            continue;
        }
        /* latest activation at or before this poll */
        uint32_t k = (sPolls - e->first) / e->period;
        uint32_t k_max = (e->last - e->first) / e->period;
        if (k > k_max) {
            k = k_max;
        }
        if (sPolls >= e->first + k * e->period + e->hold) {
            continue;
        }
        if (sPolls == e->first) {
            rt_log("input: poll %u entry %d", (unsigned)sPolls, i);
        }
        *buttons |= e->buttons;
        if (e->has_stick) {
            *x = e->x;
            *y = e->y;
        }
    }
}

uint32_t rt_input_polls(void) {
    return sPolls;
}

uint16_t rt_input_buttons(void) {
    return sButtons;
}

void rt_input_init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    if (sScriptLen < 0) {
        load_script();
    }
}

static int8_t scale_axis(int raw, bool invert) {
    int v = raw - 128;
    if (invert) {
        v = -v;
    }
    const int deadzone = 20;
    if (v > -deadzone && v < deadzone) {
        return 0;
    }
    v = v > 0 ? v - deadzone : v + deadzone;
    v = v * 80 / (127 - deadzone);
    if (v > 80) {
        v = 80;
    }
    if (v < -80) {
        v = -80;
    }
    return (int8_t)v;
}

/*
 * The controls:
 *   cross A, square B, L Z, R R, START start, triangle C-up, circle C-right,
 *   d-pad the four C buttons, analog stick stick;
 *   SELECT + d-pad the N64 d-pad (only debug screens read it), SELECT + L the
 *   N64's L, SELECT + R a debug capture of the next frame (not passed to the game).
 */
static void poll_pad(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    uint16_t b = 0;
    bool shift = (pad.Buttons & PSP_CTRL_SELECT) != 0;

    if (pad.Buttons & PSP_CTRL_CROSS) b |= N64_A;
    if (pad.Buttons & PSP_CTRL_SQUARE) b |= N64_B;
    if (pad.Buttons & PSP_CTRL_LTRIGGER) b |= N64_Z;
    if (pad.Buttons & PSP_CTRL_RTRIGGER) b |= N64_R;
    if (pad.Buttons & PSP_CTRL_START) b |= N64_START;
    if (pad.Buttons & PSP_CTRL_TRIANGLE) b |= N64_CU;
    if (pad.Buttons & PSP_CTRL_CIRCLE) b |= N64_CR;

    /*
     * SELECT + R: capture the next frame for debugging (rt_gfx_request_capture:
     * the RDRAM image the frame is rendered from, and what the PSP showed).
     * The combination is kept from the game.
     */
    static bool sCaptureHeld = false;
    bool capture = shift && (pad.Buttons & PSP_CTRL_RTRIGGER);
    if (capture) {
        b &= (uint16_t)~N64_R;
        if (!sCaptureHeld) {
            rt_gfx_request_capture();
        }
    }
    sCaptureHeld = capture;

    if (shift) {
        /* SELECT + d-pad: N64 d-pad / L */
        if (pad.Buttons & PSP_CTRL_UP) b |= N64_DU;
        if (pad.Buttons & PSP_CTRL_DOWN) b |= N64_DD;
        if (pad.Buttons & PSP_CTRL_LEFT) b |= N64_DL;
        if (pad.Buttons & PSP_CTRL_RIGHT) b |= N64_DR;
        if (pad.Buttons & PSP_CTRL_LTRIGGER) b |= N64_L;
    } else {
        if (pad.Buttons & PSP_CTRL_UP) b |= N64_CU;
        if (pad.Buttons & PSP_CTRL_DOWN) b |= N64_CD;
        if (pad.Buttons & PSP_CTRL_LEFT) b |= N64_CL;
        if (pad.Buttons & PSP_CTRL_RIGHT) b |= N64_CR;
    }

    int8_t x = scale_axis(pad.Lx, false);
    int8_t y = scale_axis(pad.Ly, true);
    if (sScriptLen > 0) {
        apply_script(&b, &x, &y);
    }
    sPolls++;
    sButtons = b;
    sStickX = x;
    sStickY = y;
}

/* ---- osCont ------------------------------------------------------------- */

/* OSContStatus[4]: one standard controller without a pak, three absent. */
static void write_cont_status(uint32_t data) {
    for (int i = 0; i < 4; i++) {
        uint32_t s = data + 4 * i;
        if (i == 0) {
            wr_u16(s, 0x0005);  /* CONT_TYPE_NORMAL */
            wr_u8(s + 2, 0x00); /* no pak */
            wr_u8(s + 3, 0);
        } else {
            wr_u16(s, 0);
            wr_u8(s + 2, 0);
            wr_u8(s + 3, CONT_NO_RESPONSE_ERROR);
        }
    }
}

/* s32 osContInit(OSMesgQueue* mq, u8* bitpattern, OSContStatus* data) */
void osContInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr_u8(ctx->r5, 0x01);
    write_cont_status(ctx->r6);
    rt_input_init();
    ctx->r2 = 0;
}

void osContStartQuery_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_post_message(ctx->r4, 0, false, true);
    ctx->r2 = 0;
}

/* void osContGetQuery(OSContStatus* data) */
void osContGetQuery_recomp(uint8_t* rdram, recomp_context* ctx) {
    write_cont_status(ctx->r4);
}

void osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    poll_pad();
    rt_post_message(ctx->r4, 0, false, true);
    ctx->r2 = 0;
}

/* void osContGetReadData(OSContPad* data) */
void osContGetReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t data = ctx->r4;
    for (int i = 0; i < 4; i++) {
        uint32_t p = data + 6 * i;
        if (i == 0) {
            wr_u16(p, sButtons);
            wr_u8(p + 2, (uint8_t)sStickX);
            wr_u8(p + 3, (uint8_t)sStickY);
            wr_u8(p + 4, 0);
        } else {
            wr_u16(p, 0);
            wr_u8(p + 2, 0);
            wr_u8(p + 3, 0);
            wr_u8(p + 4, CONT_NO_RESPONSE_ERROR);
        }
    }
}

RT_STUB_RETURN(osContSetCh_recomp, 0)

/* ---- RTC ---------------------------------------------------------------- */

#define RTC_STATUS_STOPPED 0x80

typedef struct {
    int64_t offset_seconds;
    uint8_t cr[2];
    uint8_t sram[8];
} RtcState;

static RtcState sRtc = { 0, { 0x03, 0x00 }, { 0 } };
static bool sRtcDirty = false;

static void rtc_load(void) {
    char path[256];
    SceUID fd = sceIoOpen(rt_data_path("rtc.bin", path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd >= 0) {
        RtcState loaded;
        if (sceIoRead(fd, &loaded, sizeof(loaded)) == sizeof(loaded)) {
            sRtc = loaded;
            /* The clock never stays stopped across restarts. */
            sRtc.cr[1] &= ~0x04;
        }
        sceIoClose(fd);
    }
}

static void rtc_save(void) {
    char path[256];
    SceUID fd = sceIoOpen(rt_data_path("rtc.bin", path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, &sRtc, sizeof(sRtc));
        sceIoClose(fd);
    }
    sRtcDirty = false;
}

void rt_rtc_init(void) {
    rtc_load();
}

void rt_rtc_tick(void) {
    if (sRtcDirty) {
        rt_io_begin();
        rtc_save();
        rt_io_end();
    }
}

static uint8_t to_bcd(int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int from_bcd(uint8_t v) {
    return (v >> 4) * 10 + (v & 0xF);
}

static bool rtc_stopped(void) {
    return (sRtc.cr[1] & 0x04) != 0;
}

static void rtc_read_time(uint8_t* out) {
    ScePspDateTime now;
    u64 tick;
    sceRtcGetCurrentClockLocalTime(&now);
    sceRtcGetTick(&now, &tick);
    tick += (u64)(sRtc.offset_seconds * 1000000LL);
    sceRtcSetTick(&now, &tick);

    out[0] = to_bcd(now.second);
    out[1] = to_bcd(now.minute);
    out[2] = 0x80 | to_bcd(now.hour);
    out[3] = to_bcd(now.day);
    out[4] = (uint8_t)sceRtcGetDayOfWeek(now.year, now.month, now.day);
    out[5] = to_bcd(now.month);
    out[6] = to_bcd(now.year % 100);
    out[7] = now.year >= 2000 ? 1 : 0;
}

static void rtc_write_time(const uint8_t* in) {
    ScePspDateTime target;
    memset(&target, 0, sizeof(target));
    target.second = from_bcd(in[0]);
    target.minute = from_bcd(in[1]);
    target.hour = from_bcd(in[2] & 0x7F);
    target.day = from_bcd(in[3]);
    target.month = from_bcd(in[5]);
    target.year = from_bcd(in[6]) + (in[7] != 0 ? 2000 : 1900);

    ScePspDateTime now;
    u64 now_tick;
    u64 target_tick;
    sceRtcGetCurrentClockLocalTime(&now);
    sceRtcGetTick(&now, &now_tick);
    if (sceRtcGetTick(&target, &target_tick) < 0) {
        rt_log("RTC: rejected time write %04d-%02d-%02d %02d:%02d:%02d", target.year, target.month, target.day,
               target.hour, target.minute, target.second);
        return;
    }
    sRtc.offset_seconds = ((int64_t)target_tick - (int64_t)now_tick) / 1000000LL;
    sRtcDirty = true;
    rt_log("RTC: set to %04d-%02d-%02d %02d:%02d:%02d (offset %lld s)", target.year, target.month, target.day,
           target.hour, target.minute, target.second, (long long)sRtc.offset_seconds);
}

/* Channel 4 (cartridge) joybus command. Returns false if unsupported. */
static bool cart_command(uint8_t* tx, uint8_t txsize, uint8_t* rx, uint8_t rxsize) {
    uint8_t cmd = tx[0];
    switch (cmd) {
        case 0x06: /* RTC info */
            if (rxsize < 3) {
                return false;
            }
            rx[0] = 0x00;
            rx[1] = 0x10; /* RTC present */
            rx[2] = rtc_stopped() ? RTC_STATUS_STOPPED : 0x00;
            return true;

        case 0x07: /* RTC read block */
            if (txsize < 2 || rxsize < 9) {
                return false;
            }
            memset(rx, 0, 9);
            switch (tx[1]) {
                case 0:
                    rx[0] = sRtc.cr[0];
                    rx[1] = sRtc.cr[1];
                    break;
                case 1:
                    memcpy(rx, sRtc.sram, 8);
                    break;
                case 2:
                    rtc_read_time(rx);
                    break;
                default:
                    break;
            }
            rx[8] = rtc_stopped() ? RTC_STATUS_STOPPED : 0x00;
            return true;

        case 0x08: /* RTC write block */
            if (txsize < 10 || rxsize < 1) {
                return false;
            }
            switch (tx[1]) {
                case 0:
                    sRtc.cr[0] = tx[2];
                    sRtc.cr[1] = tx[3];
                    sRtcDirty = true;
                    break;
                case 1:
                    if (!(sRtc.cr[0] & 0x01)) {
                        memcpy(sRtc.sram, tx + 2, 8);
                        sRtcDirty = true;
                    }
                    break;
                case 2:
                    if (!(sRtc.cr[0] & 0x02)) {
                        rtc_write_time(tx + 2);
                    }
                    break;
                default:
                    break;
            }
            rx[0] = rtc_stopped() ? RTC_STATUS_STOPPED : 0x00;
            return true;

        default:
            RT_LOG_ONCE("PIF: unsupported cartridge command %02X", cmd);
            return false;
    }
}

static bool controller_command(int channel, uint8_t* tx, uint8_t txsize, uint8_t* rx, uint8_t rxsize) {
    (void)txsize;
    if (channel != 0) {
        return false;
    }
    switch (tx[0]) {
        case 0x00:
        case 0xFF:
            if (rxsize >= 3) {
                rx[0] = 0x05;
                rx[1] = 0x00;
                rx[2] = 0x00;
            }
            return true;
        case 0x01:
            if (rxsize >= 4) {
                rx[0] = (uint8_t)(sButtons >> 8);
                rx[1] = (uint8_t)sButtons;
                rx[2] = (uint8_t)sStickX;
                rx[3] = (uint8_t)sStickY;
            }
            return true;
        default:
            return false;
    }
}

static uint8_t sPifRam[64];

static void process_pif(void) {
    int channel = 0;
    int i = 0;
    while (i < 63 && channel < 6) {
        uint8_t txsize = sPifRam[i];
        if (txsize == 0xFE) {
            break;
        }
        if (txsize == 0xFF || txsize == 0xFD) {
            i++;
            continue;
        }
        if (txsize == 0x00) {
            channel++;
            i++;
            continue;
        }
        txsize &= 0x3F;
        if (i + 1 >= 63) {
            break;
        }
        uint8_t rxsize = sPifRam[i + 1] & 0x3F;
        uint8_t* tx = &sPifRam[i + 2];
        uint8_t* rx = tx + txsize;
        if (i + 2 + txsize + rxsize > 64) {
            break;
        }
        bool ok;
        if (channel < 4) {
            ok = controller_command(channel, tx, txsize, rx, rxsize);
        } else {
            ok = cart_command(tx, txsize, rx, rxsize);
        }
        if (!ok) {
            sPifRam[i + 1] |= 0x80; /* no response */
        }
        i += 2 + txsize + rxsize;
        channel++;
    }
    sPifRam[63] = 0; /* command done */
}

/* s32 __osSiRawStartDma(s32 direction, void* buffer) */
void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t direction = ctx->r4;
    uint32_t buffer = ctx->r5;
    if (direction == 1) {
        rt_copy_from_rdram(buffer, sPifRam, 64);
        process_pif();
    } else {
        rt_copy_to_rdram(buffer, sPifRam, 64);
    }
    rt_post_event(OS_EVENT_SI);
    ctx->r2 = 0;
}

RT_STUB(__osSiGetAccess_recomp)
RT_STUB(__osSiRelAccess_recomp)
RT_STUB(__osSiCreateAccessQueue_recomp)
RT_STUB_RETURN(__osSiDeviceBusy_recomp, 0)

void __osSiRawReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr_w32(ctx->r5, 0);
    ctx->r2 = 0;
}

RT_STUB_RETURN(__osSiRawWriteIo_recomp, 0)

/* ---- Controller Pak / rumble / EEPROM: not present ---------------------- */

#define PFS_ERR_NOPACK 1

RT_STUB_RETURN(osPfsInitPak_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsRepairId_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsChecker_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsAllocateFile_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsDeleteFile_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsReadWriteFile_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsFileState_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsFreeBlocks_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osPfsNumFiles_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osPfsGetStatus_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osPfsRWInode_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osPfsSelectBank_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osCheckId_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osCheckPackId_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osGetId_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osRepairPackId_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osContRamRead_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osContRamWrite_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(osMotorInit_recomp, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osMotorAccess_recomp, PFS_ERR_NOPACK)

/* s32 osPfsFindFile(OSPfs*, u16, u32, u8*, u8*, s32* file_no) */
void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr_w32(rt_stack_arg(ctx, 5), (uint32_t)-1);
    ctx->r2 = PFS_ERR_NOPACK;
}

/* s32 osPfsIsPlug(OSMesgQueue* mq, u8* pattern) */
void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr_u8(ctx->r5, 0);
    ctx->r2 = 0;
}

void osEepromRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_fill_rdram(ctx->r6, 0, 8);
    ctx->r2 = (gpr)-1;
}

RT_STUB_RETURN(osEepromWrite_recomp, (gpr)-1)
RT_STUB_RETURN(__osEepStatus_recomp, 1)
RT_STUB_RETURN(osGbpakInit, PFS_ERR_NOPACK)
RT_STUB_RETURN(__osContAddressCrc_recomp, 0)
RT_STUB_RETURN(__osContDataCrc_recomp, 0)
RT_STUB(__osContGetInitData_recomp)
RT_STUB(__osPackRequestData_recomp)
RT_STUB(__osPfsGetInitData_recomp)
RT_STUB(__osPfsGetOneChannelData_recomp)
RT_STUB(__osPfsRequestData_recomp)
RT_STUB(__osPfsRequestOneChannel_recomp)
