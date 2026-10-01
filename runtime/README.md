# The runtime

N64Recomp turns every function of the game into a C function that works on a
simulated MIPS register file (`recomp_context`) and a 4 MB byte array standing
in for the N64's RDRAM. This directory is everything that code needs around
it on a PSP: the N64 operating system (libultra), the hardware the game talks
to through it, and the PSP side (threads, display, sound, memory stick,
standby).

```
runtime/
  include/
    recomp_psp.h        what generated code includes: registers, RDRAM access macros, hooks
    librecomp/sections.h  types of the generated section table
  src/
    rt.h                shared declarations, one section per source file
    main.c              entry point, boot (IPL3), exit/standby/resume
    log.c               afpsp.log, memory stick helpers, debug switch files
    sections.c          function lookup for indirect calls, overlay tracking
    sched.c             N64 threads and message queues on PSP threads
    preempt.c           preemption points in recompiled code
    timer.c             osSetTimer, osGetTime
    misc.c              the rest of libultra; the recompiler's error hooks
    pi.c                cartridge ROM reads and PI DMA
    si.c                controllers, input scripts, the cartridge clock (RTC)
    flash.c             FlashRAM saves (flash.bin)
    vi.c                retraces and framebuffer swaps
    sp.c                RSP tasks: graphics to gfx/, audio to audio.c
    audio.c             audio tasks, the AI, sound output
    audio/              the audio microcode (aspmain.c) and the Media Engine (me_audio.c, me_boot.S)
    gfx/                the renderer (see gfx/gfx_internal.h)
    data.c              opens the data files packed into EBOOT.PBP (or next to a bare PRX)
    text_en.c           English dialogue (text_en.bin)
    names_en.c          English names, items and letters (names_en.bin)
    strings_en.c        English dates and times
```

## How it fits together

**Calling convention.** A replacement for a libultra function has the same
signature as generated code, `void f(uint8_t* rdram, recomp_context* ctx)`:
arguments are in `ctx->r4`..`r7` and on the stack (`rt_stack_arg`), the
result goes in `ctx->r2`. N64Recomp names the libultra functions it leaves to
the runtime `X_recomp`; `recomp/af.jp.toml` lists the others it should leave
alone (`ignored`), and splices calls to the runtime into game functions
(`[[patches.hook]]`), which is how the English text and overlay tracking work.
`RT_STUB`/`RT_STUB_RETURN` (rt.h) declare the many that do nothing.

**Memory.** RDRAM is a 4 MB buffer holding 32-bit words in host byte order,
so word loads need no swap and byte/halfword accesses flip the low address
bits (`MEM_B`, `MEM_H` in recomp_psp.h). Use `rd_w32`/`wr_u8`/... and
`rt_copy_to_rdram`/`rt_copy_from_rdram` for big-endian byte buffers. A little
of the N64's low memory, which the game never uses, holds runtime-owned
structures (`RT_*` addresses in rt.h).

**Threads.** Every N64 thread runs on a PSP thread of its own, but only one
at a time: the running thread holds a baton, and hands it over only inside
OS calls and at preemption points (sched.c). That keeps libultra's
single-CPU, priority-based scheduling exact and makes locking around game
state unnecessary. Everything asynchronous -- retraces, timers, finished
DMAs, the renderer -- runs on helper PSP threads that post messages; the
baton holder delivers them. `scripts/recompile.sh` puts a preemption point at
the start of every recompiled function (`RECOMP_PREEMPT`, preempt.c), so a
long computation can't starve the game's audio thread.

**Graphics.** `osSpTaskStartGo` with a graphics task goes to the renderer
thread (gfx/gfx_worker.c), which interprets the display list and draws it with
sceGu; see the file list at the top of `gfx/gfx_internal.h`.

**Audio.** Audio tasks run through an HLE of Animal Forest's own audio
microcode (audio/aspmain.c), checked bit-exact against the real microcode.
On hardware they run on the Media Engine, the PSP's
second CPU (audio/me_audio.c); anywhere else on the main CPU. The samples go
to a ring buffer that a thread plays through sceAudio (audio.c).

## Files next to the EBOOT

| File | Meaning |
|---|---|
| `baserom.z64` | the ROM (any byte order); or `rom_path.txt` holding its path |
| `kcall.prx` | the Media Engine library's kernel module (real hardware only) |
| `text_en.bin`, `names_en.bin` | English dialogue and names (`scripts/make_text_en.sh`): read from the EBOOT, where the Makefile packs them; next to a bare PRX otherwise (data.c) |
| `flash.bin`, `rtc.bin` | the save and the clock offset, written by the runtime |
| `afpsp.log` | the log |

### Debug switches

Each is a file next to the EBOOT; the ones with numbers read them from the
file.

| File | Effect | Where |
|---|---|---|
| `input_script.txt` | scripted controller input (format in si.c) | si.c |
| `shot_frames.txt` | save these frames as `shot_NNNNN.bmp` | gfx_debug.c |
| SELECT + R (not a file) | save the next frame's RDRAM and picture as `capture_N*` | gfx_debug.c |
| `no_stretch.txt` | start with the picture at 4:3 between black bars (START + SELECT switches) | gfx_frame.c |
| `dump_frames.txt` | save the RDRAM of these graphics tasks | gfx_debug.c |
| `replay.txt` | `<dump> <task> [step]`: render a dump forever instead of booting | gfx_debug.c |
| `trace_tasks.txt` | log every draw of these graphics tasks | gfx_debug.c |
| `skip_draws.txt` | leave out these draws (1-based) | gfx_debug.c |
| `dump_tex.txt` | write the first 400 textures built | gfx_tex.c |
| `soften.txt` | softening strength in percent (0 = off; default 30) | gfx_frame.c |
| `no_snap.txt`, `no_weld.txt`, `no_cut.txt`, `no_split.txt`, `hard_edges.txt` | turn off vertex snapping, welding of nearly coincident vertices, stencil cut-outs, split combiners, soft texture edges | gfx_draw.c, gfx_combiner.c |
| `no_yield.txt` | ignore the game's graphics task yields | gfx_worker.c |
| `no_me.txt` | run audio on the main CPU | audio/me_audio.c |
| `sync_log.txt` | write every log line before going on (for a crash that loses the last lines) | log.c |
| `audio_wav.txt` | `start length` (s): record the output to `afpsp_audio.wav` | audio.c |
| `dump_audio.txt` | dump these audio tasks' RDRAM as `aspt_<n>.bin` | audio.c |

## Reading the log

Every 120 frames the renderer logs where the time went, and every 600 audio
tasks the audio side does:

- `idle N% of the CPU` -- what nothing wanted, measured by an idle thread.
- `gfx: render N% of wall time (N us a task, N us of them on the CPU)` -- the
  renderer's share, its time per graphics task (the number to compare when
  optimising it, on a replayed dump) and how much of that it was running
  rather than waiting for the game's threads to leave it the CPU; then
  framebuffer captures, render targets, texture builds and how long handing
  over a task waited.
- `frame N (poll P): ...` -- per-frame counts, then `busy` (game threads; also
  as time per frame, the number to compare for the game's own code),
  `gfx`, `blocked` (the game waiting for the renderer) and `audio` shares,
  and per game thread `t<id> <CPU>%/w<avg>,<worst wait>/h<longest hold>,<calls>`.
- `audio rate: game N Hz, hardware N Hz` -- both should be ~32000.
- `audio: ... underruns N ...` -- underruns are gaps in the sound;
  `retraces lost` are audio frames the game never made.
- `audio pacing: ...` -- the spacing of audio tasks and how long the game's
  audio cycle took to react and dispatch.
- `gfx approx: ...` -- a combiner the renderer can only approximate (once each).
