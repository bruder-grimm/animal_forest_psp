# Animal Forest on PSP

## Building

This repository contains no game data (only two title-screen screenshots,
used as the EBOOT's icon and background). You supply your own dumps:

- the **Japanese N64 *Animal Forest*** ROM (NUS-NAFJ; MD5
  `a4f7c57c180297b2e7ba5a5feb44fe0b` as big-endian `.z64`), which the game's
  code is recompiled from
- the **English fan translation** ROM *Animal Forest (U) [!]* (MD5
  `f827d11ee513d5edde44a3a9598f0934`), which the game plays
- the **European GameCube *Animal Crossing*** disc image (GAFP01; `.iso`,
  `.gcm` or `.ciso`), for the English text

### 1. Install the tools

- [PSPDEV](https://pspdev.github.io/) toolchain, in `~/pspdev` or at `$PSPDEV`
- [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core),
  built and installed into `$PSPDEV/psp` (keep its `kcall.prx`)
- the prerequisites of the [zeldaret/af](https://github.com/zeldaret/af)
  decomp: MIPS binutils (`mips-linux-gnu-*`), clang, Python 3
- GNU make 4 (on macOS: Homebrew's `make`, which installs as `gmake`), git,
  CMake, Ninja, a C++20 compiler

### 2. Build

```bash
./build.sh "Animal Crossing (Europe).iso" "Doubutsu no Mori (Japan).z64" "Animal Forest (U) [!].z64"
```

The ROMs can be in any byte order. The first run takes a while: it clones and
builds the decomp and N64Recomp, recompiles the game and builds the English
text. Later runs only redo what changed.

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
[N64Recomp](https://github.com/N64Recomp/N64Recomp) is Mr-Wiseguy's; the
runtime's thread model follows his N64ModernRuntime. The Media Engine support
uses mcidclan's
[psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core).
The English text comes from your own copy of the GameCube *Animal Crossing*
and the fan translation ROM; `tools/text_en_manual.txt`
holds translations made for this port of the messages the GameCube release
has no counterpart for (AI, be aware!).
