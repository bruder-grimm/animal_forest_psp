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
    files.c             files next to the EBOOT: reading, writing, debug switches, data packed into the EBOOT
    log.c               afpsp.log
    sections.c          function lookup for indirect calls, overlay tracking
    sched.c             N64 threads and message queues on PSP threads
    ctx.S               register contexts for the game threads' stacks
    capture.c           SELECT + R captures of the whole game, and resuming from one
    preempt.c           preemption points in recompiled code
    spram.c             runs the renderer's hottest functions from the PSP's scratchpad
    native_math.c       the game's matrix, trig, keyframe and field-info functions, natively
    timer.c             osSetTimer, osGetTime
    misc.c              the rest of libultra; the recompiler's error hooks
    pi.c                cartridge ROM reads and PI DMA
    si.c                controllers, input scripts, the cartridge clock (RTC)
    flash.c             FlashRAM saves (flash.bin)
    vi.c                retraces and framebuffer swaps
    sp.c                RSP tasks: graphics to gfx/, audio to audio.c
    audio.c             audio tasks, the AI, sound output
    audio/              the audio microcode (aspmain.c), the Media Engine (me_audio.c, me_boot.S),
                        the output's rate conversion (resample.c)
    gfx/                the renderer (see gfx/gfx_internal.h)
    english/            the game in English: dialogue, names, letters, menus, typing, dates
                        (see english/english.h)
```

## How it fits together

**Calling convention.** A replacement for a libultra function has the same
signature as generated code, `void f(uint8_t* rdram, recomp_context* ctx)`:
arguments are in `ctx->r4`..`r7` and on the stack (`rt_stack_arg`), the
result goes in `ctx->r2`. N64Recomp names the libultra functions it leaves to
the runtime `X_recomp`; `recomp/af.jp.toml` lists the others it should leave
alone (`ignored`) or replaces with its own (`native`), and splices calls to
the runtime into game functions (`[[patches.hook]]`), which is how the
English text (`rt_en_*`) and overlay tracking work.
`RT_STUB`/`RT_STUB_RETURN` (rt.h) declare the many that do nothing.

**Registers.** The generated code keeps the MIPS registers in C locals
(`register_locals` in the toml, `RL_*` in recomp_psp.h) and trusts the
calling convention: at a call only a0-a3 and sp are written to `ctx`, after
it only v0/v1 are read back, and a return writes v0/v1. So a replacement
reads its arguments from `ctx` and writes its result there as before, but it
must not expect the caller's other registers in `ctx`, and a callee's change
to s0-s8 or sp never reaches the caller. Hooks are different: around one the
whole register file goes through `ctx`, so `[[patches.hook]]` text may read
and write any register. The recompiler reports code that doesn't follow the
convention as `ABI:` lines in logs/recompile.log (five known, harmless).

**Natives.** The game functions in the toml's `native` list (matrix, trig,
keyframe and field-info code that runs hundreds of times a frame) are not
recompiled: native_math.c defines them under the same names, with
bit-identical results.

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
state unnecessary. The PSP threads run on stacks of the runtime's own,
reserved straight after the module before the C library's heap, and each
saves its register context when it waits for the baton -- which is what
makes a capture resumable. Everything asynchronous -- retraces, timers, finished
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
| `text_en.bin`, `names_en.bin` | English dialogue and names (`scripts/make_text_en.sh`): read from the EBOOT, where the Makefile packs them; next to a bare PRX otherwise (files.c) |
| `flash.bin`, `rtc.bin` | the save and the clock offset, written by the runtime |
| `capture_N.state`, `capture_N.bmp` | captures (SELECT + R, see "Captures" below) |
| `resume_flash.bin` | the save of a game resumed from a capture (`flash.bin` is left alone then) |
| `afpsp.log` | the log, written only if `log.txt` is there (see the debug switches) |

### Debug switches

Each is a file next to the EBOOT; the ones with numbers read them from the
file.

| File | Effect | Where |
|---|---|---|
| `input_script.txt` | scripted controller input (format in si.c) | si.c |
| `shot_frames.txt` | save these frames (up to 64) as `shot_NNNNN.bmp`; a frame nothing was drawn for gives the next one that was | gfx_debug.c |
| SELECT + R (not a file) | a capture: `capture_N.state` and `capture_N.bmp` (see "Captures") | capture.c |
| `auto_capture.txt` | take captures at these controller polls, as SELECT + R would (polls count from the resume in a resumed game) | si.c |
| `resume.txt` | `capture_N.state`: start the game from that capture instead of booting it | capture.c |
| `no_stretch.txt` | start with the picture at 4:3 between black bars (START + SELECT switches) | gfx_frame.c |
| `dump_frames.txt` | save the RDRAM of these graphics tasks | gfx_debug.c |
| `replay.txt` | `<dump> <task> [step]`: render a dump forever instead of booting; the dump may be a capture, with task 0 for its own | gfx_debug.c |
| `trace_tasks.txt` | log every draw of these graphics tasks | gfx_debug.c |
| `skip_draws.txt` | leave out these draws (1-based) | gfx_debug.c |
| `dump_tex.txt` | write the first 400 textures built | gfx_tex.c |
| `soften.txt` | softening strength in percent (0 = off; default 30) | gfx_frame.c |
| `cpu_mhz.txt` | `222` or `266`: run the CPU at that clock instead of 333 (to see what a slower PSP would do) | main.c |
| `no_snap.txt`, `no_weld.txt`, `no_cut.txt`, `no_split.txt`, `hard_edges.txt`, `no_upright.txt` | turn off vertex snapping, welding of nearly coincident vertices, stencil cut-outs, split combiners, soft texture edges, keeping ground decals (shadows) off upright surfaces | gfx_draw.c, gfx_combiner.c |
| `no_target_tex.txt` | never sample a render target where it is in VRAM: copy it back to RDRAM and build a texture | gfx_target.c |
| `no_yield.txt` | ignore the game's graphics task yields | gfx_worker.c |
| `no_spram.txt` | leave the renderer's hot functions in RAM instead of running them from the scratchpad | spram.c |
| `no_me.txt` | run audio on the main CPU | audio/me_audio.c |
| `no_me_resample.txt` | convert the output's rate on the main CPU, with the audio tasks still on the Media Engine | audio/resample.c |
| `log.txt` | write the log (`afpsp.log`, and to PSPLink's shell); without it nothing is logged, and only a fatal stop leaves its reason in `afpsp.log` | log.c |
| `sync_log.txt` | the log, with every line written before going on (for a crash that loses the last lines) | log.c |
| `audio_wav.txt` | `start length` (s): record the output to `afpsp_audio.wav` | audio.c |
| `dump_audio.txt` | dump these audio tasks' RDRAM as `aspt_<n>.bin` | audio.c |

## Captures

SELECT + R (or `auto_capture.txt`) asks for a capture, which is taken when the
game next hands over a graphics task, before the renderer gets it: the
renderer, the Media Engine and the cartridge DMA thread are first left to
finish what they have, and the other game threads are all waiting for the
baton. `capture_N.state` then holds RDRAM -- the RDRAM that task is rendered
from, so it is also its replay dump -- the game threads' stacks and register
contexts, and the runtime's part of the machine: threads, message queues and
posted messages, registered events, timers, the overlay table, the save, the
cartridge clock and the PIF, the audio microcode's state, and what the
renderer owes the game and knows about its framebuffers. `capture_N.bmp` is
the frame drawn from that task. Taking one holds the game for about 0.2 s
over PSPLink, longer on a memory stick (a short gap in the sound). Captures
get the first number that no `capture_N.state` or `.bmp` there has yet.

`resume.txt` naming a capture starts the game from it: the threads come back
where they were waiting, and the first frame drawn is the captured one, so
`trace_tasks.txt` with task 1 traces it; `shot_frames.txt`, `dump_frames.txt`,
input scripts and `auto_capture.txt` count frames, tasks and polls from the
resume. The game keeps the clock it had in the capture and saves to
`resume_flash.bin`. Only the build that took a capture can resume it (the
stacks hold its code and data addresses; the log says why if it refuses).
`tools/afstate.py info` shows a capture's header and parts and whether
build/psp can resume it; `tools/afstate.py rdram` writes its RDRAM as
`capture_N_<task>.bin` for `replay.txt`.

Adding state to the runtime that the game depends on (anything a game
resumed from a capture would get wrong without it) means adding it to the
file's `rt_*_capture` function, which capture.c calls in order.

## Reading the log

(With `log.txt` next to the EBOOT; the test scripts put it there.)
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
