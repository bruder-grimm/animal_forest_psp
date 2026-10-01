# Building

This repository contains no game data -- no ROM, no game text, no extracted
graphics (the EBOOT's icon and background in `assets/` are screenshots of the
port's title screen). You supply your own dumps by putting them in the `roms/`
folder, and one script builds everything from them:

```bash
./build.sh
```

Each file is recognised by its content (`scripts/find_inputs.py`), so its name
does not matter, nor where it is in `roms/`, and a ROM can be in any byte order.
`./build.sh` says what it made of every file and what is missing; files named on
the command line (`./build.sh ~/dumps/disc.iso ...`) are looked at too. The
files are:

- the **European GameCube *Animal Crossing*** (GAFP01; `.iso`, `.gcm` or
  `.ciso`): the English dialogue, names and letters;
- the **Japanese N64 *Animal Forest*** (NUS-NAFJ; MD5
  `a4f7c57c180297b2e7ba5a5feb44fe0b` as big-endian `.z64`): the game's code
  is recompiled from it;
- optionally the **English fan translation** *Animal Forest (U) [!]* (MD5
  `f827d11ee513d5edde44a3a9598f0934`), a patch of the Japanese ROM: the ROM
  the game plays, with its English title logo, menus and screens. Without it
  the game plays the Japanese ROM: the dialogue, names and letters are still
  English (they come from the disc), but the title screen, menus and signs
  stay Japanese. The two builds differ in the recompiled code too (see
  section 2), so going from one to the other recompiles the game.

`roms/README.md` has the MD5 of every accepted ROM dump (all three byte orders)
and says what else is recognised and refused.

It leaves `dist/AFPSP/` -- `EBOOT.PBP` (with the English text packed into
it) and `baserom.z64` -- ready to copy to a PSP or PPSSPP ([Install](#7-install)).
The first run takes a while (it clones and builds the decomp and N64Recomp);
later runs only redo what changed. It needs the tools of section 1, or
[Docker](#building-with-docker) instead.

The rest of this file describes what `build.sh` does, step by step, for
working on the port.

The build has four stages, each feeding the next:

```
your ROM ─► zeldaret/af decomp ─► animalforest-jp.elf ─► N64Recomp (PSP patch) ─► work/recomp_out/*.c ─┐
                                                                                                     ├─► psp-gcc ─► EBOOT.PBP
                                                                  runtime/ (the PSP runtime) ────────┘
```

The first three only need re-running when the decomp, the N64Recomp patch or
`recomp/af.jp.toml` change; day to day it is just `gmake`.

It was developed on macOS (Apple Silicon); Linux should work with the same
tools. Everything that is downloaded or generated goes under `work/`, which
is not tracked.

## Building with Docker

If installing the tools below is more than you want, `docker-build.sh` runs
`build.sh` in a container that has all of them. The only thing to install is
[Docker](https://docs.docker.com/get-docker/):

```bash
./docker-build.sh
```

It finds the dumps in `roms/` like `build.sh` does (and takes the same optional
file arguments) and leaves the same `dist/AFPSP/`, owned by you, with `kcall.prx`
included (the image has psp-media-engine-custom-core's). Files named on the
command line are mounted read-only.

- The `Dockerfile` pins everything: Ubuntu 24.04, the PSPDEV release
  (v20260701, psp-gcc 15.2, checked against a SHA-256: the one the port was
  developed and tested on a PSP with) and the psp-media-engine-custom-core
  commit. It holds no game data and not even this
  repository, which is mounted when it runs.
- The image is `linux/amd64`, because the decomp downloads x86-64 Linux builds
  of the IDO compilers. On an Apple Silicon Mac or other ARM machine Docker
  emulates it, which is slower but works: a whole first build (decomp,
  N64Recomp, the game, the EBOOT) took about ten minutes on 4 cores that way.
  Give Docker's VM enough memory (4 GB or more) and as many cores as you can
  spare; `JOBS=n` limits the parallel jobs.
- The decomp, N64Recomp and the object files live in two Docker volumes,
  `afpsp-work` and `afpsp-build`, not in `work/` and `build/` on the host, so a
  host build and a Docker build cannot disturb each other, and a later run only
  redoes what changed. `docker volume rm afpsp-work afpsp-build` starts from
  scratch; `docker run --rm -it --entrypoint bash -v afpsp-work:/src/work
  -v afpsp-build:/src/build afpsp-build` opens a shell in them.
- Without a shell script (Windows without WSL): `docker build --platform
  linux/amd64 -t afpsp-build .`, then `docker run --rm --platform linux/amd64
  -v "%cd%:/src" -v afpsp-work:/src/work -v afpsp-build:/src/build
  afpsp-build`, with the dumps in `roms\`.

## 1. Tools

| What | Needed for | Notes |
|---|---|---|
| [PSPDEV](https://pspdev.github.io/) toolchain | the EBOOT | `psp-gcc`, PSPSDK, `pack-pbp`, `mksfoex`, `psp-prxgen`. Found at `$PSPDEV`, else `~/pspdev`. Built with psp-gcc 15.2. |
| [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core) | the EBOOT | Build it and install into `$PSPDEV/psp`: the Makefile links `-lme-core` and includes `<me-core-mapper/me-core.h>`. Keep its `kcall.prx` for installing on a PSP. |
| GNU make 4 | everything | On macOS, Homebrew's `make`, run as `gmake` (Apple's make 3.81 is too old). |
| The AF decomp's prerequisites | stage 2 | See the decomp's own README: MIPS binutils (`mips-linux-gnu-*`), python3, clang, git. On macOS, `scripts/build_n64.sh` expects the binutils in `/opt/cross/bin` and uses `mips-linux-gnu-ar` (the decomp's Makefile asks for `gar`). |
| git, CMake, Ninja, a C++20 compiler | stage 3 | To build N64Recomp. |
| Python 3 | the ROM and text tools | Standard library only. |

## 2. The ROMs

You need your own dumps.

`./build.sh` does all of this by itself (`scripts/find_inputs.py`); to do it by
hand, `prepare_rom.py` identifies a ROM by its MD5, in any byte order, and
normalises it to where the build expects it. `prepare_rom.py --list` shows the
hashes it knows (also in `roms/README.md`).

- **Japanese *Animal Forest*** (NUS-NAFJ) -- the decomp is built from it
  (`a4f7c57c180297b2e7ba5a5feb44fe0b` as big-endian `.z64`):

  ```bash
  scripts/prepare_rom.py path/to/your/rom.z64            # -> work/af/baseroms/jp/baserom.z64
  ```

- **The English fan translation** *Animal Forest (U) [!]* (MD5
  `f827d11ee513d5edde44a3a9598f0934`), optional: a data-only patch of the
  Japanese ROM, and the ROM the port plays when you have it.
  `recomp/af.jp.toml` applies its five changed instructions to the recompiled
  code, so the generated code is specific to the ROM played. `prepare_rom.py`
  recognises it too:

  ```bash
  scripts/prepare_rom.py "path/to/Animal Forest (U) [!].z64"   # -> work/af/baseroms/en/baserom.z64
  ```

  Without it the game plays the Japanese ROM, and step 4 is run as
  `scripts/recompile.sh jp`, which leaves those five instructions out
  (everything between the `fan-translation` markers in the toml).
  `work/recomp_out/rom.txt` records which of the two the generated code is for,
  and `gmake install` copies that ROM; `BASEROM=...` overrides it.

## 3. Build the decomp

```bash
scripts/build_n64.sh
```

This clones zeldaret/af into `work/af` (at the commit in
`recomp/af-decomp-commit.txt`) if it is not there, sets up the decomp's
Python environment, extracts the ROM and builds it; it should end with `animalforest-jp.z64: OK` (and the same for the
compressed ROM), and leaves `work/af/build/animalforest-jp.elf`. The full log
is `logs/n64_build.log`.

## 4. Recompile the game

```bash
scripts/setup_recomp.sh    # clones N64Recomp at recomp/n64recomp-base-commit.txt, applies recomp/n64recomp-psp.patch, builds it
scripts/recompile.sh       # work/af/build/animalforest-jp.elf -> work/recomp_out/ (for the fan translation ROM; `recompile.sh jp` for the Japanese ROM)
```

`recompile.sh` prints the function count and any functions it could not
recompile (log: `logs/recompile.log`), and gives every recompiled function a
preemption point (see `runtime/README.md`).

## 5. English dialogue

The fan translation leaves most dialogue in Japanese (and the Japanese ROM all
of it). From your own copy of the European GameCube *Animal Crossing*
(GAFP01, `.iso` or `.ciso`), this builds the English dialogue, names and
letters for the port:

```bash
scripts/make_text_en.sh "path/to/Animal Crossing (Europe) (En,Fr,De,Es,It).ciso"
```

It writes `work/text/text_en.bin` and `work/text/names_en.bin`, which the
next step packs into the EBOOT. They take precedence over the ROM's text
wherever they have an entry; `gmake` warns if they are missing, since the
game then shows only the ROM's own text. Messages the GameCube
release has no usable counterpart for come from `tools/text_en_manual.txt`,
translated for this port.

## 6. Build the EBOOT

```bash
gmake -j10
```

The result is `build/psp/EBOOT.PBP` (and `build/psp/afpsp.prx`, the same
program for PSPLink). The generated code takes a few minutes the first time;
after that only what changed is rebuilt.

| Variable | Default | Meaning |
|---|---|---|
| `PSPDEV` | `~/pspdev` | the PSP toolchain |
| `BASEROM` | the ROM `work/recomp_out/rom.txt` names (`en` unless recompiled with `jp`) | the ROM `gmake install` copies |
| `TEXT_EN`, `NAMES_EN` | `work/text/*.bin` | the English text `gmake install` copies |
| `PPSSPP_GAME_DIR` | `~/.config/ppsspp/PSP/GAME/AFPSP` | where `gmake install` installs |
| `GEN_OPT` | `-O2` | optimisation of the generated code (`-Os` was slower) |

`gmake clean` removes `build/psp`.

## 7. Install

**PPSSPP.** `gmake install` copies the EBOOT, the ROM and the English text
into PPSSPP's memory stick (`PPSSPP_GAME_DIR`). PPSSPP doesn't emulate the
Media Engine; the runtime notices and runs the audio on the main CPU (a
`no_me.txt` next to the EBOOT forces that).

**A real PSP** needs custom firmware (tested: ARK on 6.60, PSP-1000 and
PSP-2000). Copy `dist/AFPSP` from `build.sh` to `ms0:/PSP/GAME/` and add
`kcall.prx`; or, by hand, copy these into `ms0:/PSP/GAME/AFPSP/`:

- `build/psp/EBOOT.PBP`
- `kcall.prx` from psp-media-engine-custom-core (the Media Engine's kernel helper)
- the ROM, named `baserom.z64`

The English text is inside the EBOOT (its `DATA.PSAR` section, which the
Makefile fills from `work/text/`), so there are no text files to copy; the
log's `text_en:` and `names_en:` lines say where the text came from. Only a
PRX run on its own (PSPLink) reads `text_en.bin` and `names_en.bin` next to
it: copy them there from `work/text/`.

The save (`flash.bin`) and clock offset (`rtc.bin`) are written next to them.
Controls: cross A, square B, L Z, R R, START start, triangle and circle C-up
and C-right, d-pad and stick as themselves; SELECT + d-pad gives the four C
buttons and SELECT + L the N64's L. START + SELECT switches between the
picture stretched over the whole screen and at its own 4:3 shape between black
bars (a file `no_stretch.txt` next to the EBOOT starts the game at 4:3).

## Debugging

The runtime has a set of debug switches (files next to the EBOOT: scripted
input, screenshots, frame dumps and replays). One of them, an empty file
`log.txt`, makes it write a log to `afpsp.log` next to the EBOOT; without it
nothing is logged. `runtime/README.md` lists them and explains the log.

## Troubleshooting

- **`env: bash\r: No such file or directory`** (or `python3\r`, `\r: command
  not found`, a patch that won't apply) -- the files have Windows (CRLF) line
  endings, usually from Git for Windows' `core.autocrlf=true`. The repo's
  `.gitattributes` forces LF, so a fresh `git clone` of a current checkout is
  fine; for an existing copy, convert it in place:

  ```bash
  git config core.autocrlf false
  git rm --cached -r -q . && git reset --hard -q   # re-checkout as LF (drops local edits)
  ```

  (No git history, e.g. a zip? `sed -i 's/\r$//' build.sh scripts/* tools/*
  Makefile recomp/*` does it.) Under WSL, keep the checkout on WSL's own
  filesystem (`~/`), not `/mnt/c/...`.
- **`Missing: the Japanese N64 Animal Forest ROM ...`** (or the disc) -- the
  file is not in `roms/`, or is not what it looks like. `scripts/find_inputs.py`
  prints what it makes of every file there; a ROM it does not know shows its MD5,
  to compare with the table in `roms/README.md`. Archives (`.zip`, `.7z`) have to
  be unpacked first.
- **`Missing work/recomp_out: run scripts/recompile.sh first`** -- stages 3
  and 4 haven't run yet (`./build.sh` runs them all).
- **`MD5 ... is not an Animal Forest ROM this port knows`** -- the ROM is
  neither the Japanese release nor the fan translation named above (the 32 MB
  English ROM with header `NAFE` is a different build and is not supported).
- **`make: *** No rule` or odd syntax errors from make** -- that's Apple's
  make; use `gmake`.
- **`psp-gcc: command not found`** -- set `PSPDEV`, or install the toolchain in
  `~/pspdev`.
- **undefined `meLib*`/`kcall` symbols** -- psp-media-engine-custom-core isn't
  installed into `$PSPDEV/psp`.
- **The decomp build fails** -- check `logs/n64_build.log`. A vanilla N64
  build that doesn't match points at the ROM or the MIPS toolchain, not at
  this port.
- **The game shows "ROM not found"** -- `baserom.z64` must sit next to the
  EBOOT, or `rom_path.txt` there must name it.
