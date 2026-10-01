# Animal Forest on PSP

## Building

This repository contains no game data (only two title-screen screenshots,
used as the EBOOT's icon and background). You supply your own dumps, by putting
them in the [`roms/`](roms/README.md) folder; the build finds them by their
content, so what they are called does not matter:

- the **Japanese N64 *Animal Forest*** ROM (NUS-NAFJ; `.z64`, `.v64` or `.n64`),
  which the game's code is recompiled from
- optionally the **English fan translation** ROM *Animal Forest (U) [!]*, which
  the game then plays, with its English title logo, menus and screens. Without
  it the game plays the Japanese ROM: the dialogue, names and letters are still
  English, but the title screen, menus and signs stay Japanese
- the **European GameCube *Animal Crossing*** disc image (GAFP01; `.iso`,
  `.gcm` or `.ciso`), for the English text

[`roms/README.md`](roms/README.md) lists the accepted MD5s.

### 1. Install the tools

(Or skip steps 1 and 2 and [build in Docker](#or-build-in-docker): the only thing
to install is Docker.)

- [PSPDEV](https://pspdev.github.io/) toolchain, in `~/pspdev` or at `$PSPDEV`
- [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core),
  built and installed into `$PSPDEV/psp` (keep its `kcall.prx`)
- the prerequisites of the [zeldaret/af](https://github.com/zeldaret/af)
  decomp: MIPS binutils (`mips-linux-gnu-*`), clang, Python 3
- GNU make 4 (on macOS: Homebrew's `make`, which installs as `gmake`), git,
  CMake, Ninja, a C++20 compiler

### 2. Build

```bash
./build.sh
```

It says what it made of each file in `roms/` and what is missing. The first run
takes a while: it clones and builds the decomp and N64Recomp, recompiles the
game and builds the English text. Later runs only redo what changed.

### Or: build in Docker

```bash
./docker-build.sh
```

The same files in `roms/` and the same result as `build.sh` (`dist/AFPSP/`, with
`kcall.prx` already in it), but every tool comes from a Docker image, built
for you by the script. See [BUILDING.md](BUILDING.md#building-with-docker).

### 3. Install

The build leaves `dist/AFPSP/` (`EBOOT.PBP`, with the English text inside
it, and `baserom.z64`).

- **PSP** (custom firmware): copy `dist/AFPSP` to `ms0:/PSP/GAME/` and put
  `kcall.prx` in it. `KCALL_PRX=path/to/kcall.prx ./build.sh ...` copies it
  for you.
- **PPSSPP**: copy `dist/AFPSP` into its `PSP/GAME/` folder.

[BUILDING.md](BUILDING.md) describes the individual build stages and options.

## AI Note

This project was *heavily* supported by claude as copilot, so much so that I honestly don't feel comfortable taking any credit for this work.

## Credit that is actually due

The decompilation is [zeldaret/af](https://github.com/zeldaret/af)'s.
[N64Recomp](https://github.com/N64Recomp/N64Recomp) is Mr-Wiseguy's
Theruntime's thread model follows his N64ModernRuntime. 
Heavily inspired by how z2442's [oot](https://github.com/z2442/oot-PSP) and [sm64](https://github.com/z2442/sm64-port) ports.
The Media Engine support uses mcidclan's [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core).
The English text comes from your own copy of the GameCube *Animal Crossing*
and the fan translation ROM; `tools/text_en_manual.txt`
holds translations made for this port of the messages the GameCube release
has no counterpart for (AI, be aware!).
