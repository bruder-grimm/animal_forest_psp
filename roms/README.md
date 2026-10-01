# roms/

Put your game dumps in this folder, then run `./build.sh` (or `./docker-build.sh`).
The files are never committed (`.gitignore` keeps everything here but this
file out of git), and none of them is ever modified.

Every file is recognised by what is in it, so its name does not matter, and neither
does where in this folder (subfolders are fine) or, for the ROMs, the byte order.
`./build.sh` says what it made of each file.

| What | Files | |
|---|---|---|
| **European GameCube *Animal Crossing*** (game ID GAFP01) | `.iso`, `.gcm` or `.ciso` | required: the English dialogue, names and letters |
| **Japanese N64 *Animal Forest*** (*Doubutsu no Mori*, NUS-NAFJ) | `.z64`, `.v64` or `.n64` | required: the game's code is recompiled from it |
| **English fan translation** *Animal Forest (U) [!]* | `.z64`, `.v64` or `.n64` | optional: with it the game has its English title logo, menus and screens, without it those stay Japanese |

Unzip archives (`.zip`, `.7z`, ...) first. Other versions are not supported: the
USA and Japanese GameCube discs, the 32 MB English N64 ROM (game code `NAFE`) and
the decompressed ROM the decomp builds are recognised and refused with a message.

## ROM hashes

A ROM is identified by the MD5 of the file, in any of the three byte orders the
dumps circulate in. The Japanese game was released once (cartridge version 1.0),
so these are all the hashes a good dump of it has:

| | `.z64` (big-endian) | `.v64` (byte-swapped) | `.n64` (little-endian) |
|---|---|---|---|
| Japanese | `a4f7c57c180297b2e7ba5a5feb44fe0b` | `a6ef34bd225f22bbf737d61b839ae1b0` | `5a6a8590b4a318b7374a6d216c5914c6` |
| Fan translation | `f827d11ee513d5edde44a3a9598f0934` | `dce0ba571c24472dbc41f6a5b5ae4317` | `c8ecffb86bc622dbbe6a4280cb36b645` |

A dump that some tool padded out to 32 or 64 MB is accepted too: the ROM is its
first 16 MB, which is what is checked and all that is used. `scripts/prepare_rom.py --list`
prints this table, and `scripts/find_inputs.py` shows what it makes of every file
here without building anything.
