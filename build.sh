#!/usr/bin/env bash
# Builds the Animal Forest PSP port from your own game dumps, start to finish.
#
#   ./build.sh <Animal Crossing GameCube disc> <Japanese N64 ROM> [<fan translation N64 ROM>]
#
#   GameCube disc    the European Animal Crossing (GAFP01), .iso/.gcm/.ciso:
#                    the English dialogue, names and letters
#   Japanese ROM     Animal Forest (NUS-NAFJ): the game's code is recompiled
#                    from it
#   fan translation  optional: "Animal Forest (U) [!]", a patch of the Japanese
#                    ROM, which the game then plays (English logo, menus and
#                    screens). Without it the game plays the Japanese ROM: the
#                    dialogue, names and letters are still English, but the
#                    title screen, menus and signs are Japanese.
#
# The ROMs can be in any byte order and either order; they are told apart by
# their MD5 (scripts/prepare_rom.py). The build is specific to the ROM the game
# plays: switching between the two recompiles the game.
#
# The repo holds no game data (bar the EBOOT's icon and background, screenshots
# of the port's title screen): everything comes from these files. The result
# is dist/AFPSP/, the folder to copy to ms0:/PSP/GAME/ (or PPSSPP's PSP/GAME/).
#
# Tools needed (BUILDING.md has the details): the PSPDEV toolchain with
# psp-media-engine-custom-core, MIPS binutils and the other prerequisites of
# the zeldaret/af decomp, GNU make 4, git, CMake, Ninja, a C++20 compiler,
# Python 3. Everything downloaded or generated goes under work/ and build/;
# a second run only redoes what changed.
#
# Environment: PSPDEV (default ~/pspdev), JOBS (default: all cores),
# KCALL_PRX (path to psp-media-engine-custom-core's kcall.prx, copied into dist).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

usage() { sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 1; }
[ $# -ge 2 ] && [ $# -le 3 ] || usage
DISC="$1"; shift
for f in "$DISC" "$@"; do
    [ -f "$f" ] || { echo "No such file: $f"; exit 1; }
done

export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PSPDEV/bin:$PATH"
export JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
MAKE="$(command -v gmake || command -v make || true)"
OUT="$ROOT/dist/AFPSP"

step() { printf '\n== %s\n' "$*"; }

# --- tools ---------------------------------------------------------------
missing=()
for t in git python3 cmake ninja perl psp-gcc pack-pbp mksfoex psp-prxgen psp-fixup-imports; do
    command -v "$t" > /dev/null || missing+=("$t")
done
[ -n "$MAKE" ] && "$MAKE" --version 2>/dev/null | grep -q "GNU Make [4-9]" || missing+=("GNU make 4 (gmake)")
[ -f "$PSPDEV/psp/lib/libme-core.a" ] || missing+=("psp-media-engine-custom-core (libme-core.a in $PSPDEV/psp/lib)")
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing tools (see BUILDING.md):"
    printf '  %s\n' "${missing[@]}"
    exit 1
fi

# --- the inputs ----------------------------------------------------------
step "Checking the disc and the ROMs"
PYTHONPATH="$ROOT/tools" python3 - "$DISC" <<'EOF'
import sys
import gc_disc
try:
    img = gc_disc.open_image(sys.argv[1])
    game = img.disc.read(0, 6)
except Exception as e:
    sys.exit(f"{sys.argv[1]}: not a GameCube disc image ({e})")
if game != b"GAFP01" or "tgc/forest_Eng_Final_PAL50.tgc" not in img.files:
    sys.exit(f"{sys.argv[1]}: this is {game!r}; the European Animal Crossing (GAFP01) is needed")
print("disc: Animal Crossing (Europe), GAFP01")
EOF
ROMS="$(python3 scripts/prepare_rom.py "$@")"
echo "$ROMS" | sed 's/^/rom: /'
echo "$ROMS" | grep -q '^jp ' || {
    echo "The Japanese Animal Forest ROM is missing: the game's code is recompiled from it."
    exit 1
}
# The ROM the game plays, and so the variant of the recompiled code.
if echo "$ROMS" | grep -q '^en '; then
    ROM=en
else
    ROM=jp
    echo "No English fan translation ROM (Animal Forest (U) [!]) given: the game will play the"
    echo "Japanese ROM, with the English dialogue from the disc but Japanese title screen and menus."
fi

# --- the decomp: work/af/build/animalforest-jp.elf -----------------------
ELF="$ROOT/work/af/build/animalforest-jp.elf"
if [ -f "$ELF" ]; then
    step "Decomp already built ($ELF)"
else
    step "Building the zeldaret/af decomp from your ROM (takes a while; log: logs/n64_build.log)"
    scripts/build_n64.sh
    [ -f "$ELF" ] || { echo "The decomp build left no $ELF"; exit 1; }
fi

# --- the recompiler and the game as C: work/recomp_out -------------------
step "Building N64Recomp with the PSP patch"
scripts/setup_recomp.sh

# Recompiling rewrites every generated file (and so rebuilds all of them):
# only when the ELF, the configuration or the recompiler changed.
STAMP="$ROOT/work/recomp_out/stamp.txt"
stamp="$(cat "$ELF" recomp/af.jp.toml recomp/overlays.txt recomp/n64recomp-psp.patch \
    recomp/n64recomp-base-commit.txt scripts/recompile.sh | cksum)"
[ "$ROM" = en ] || stamp="$stamp $ROM"
if [ -f work/recomp_out/funcs.h ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$stamp" ]; then
    step "Recompiled code is up to date (work/recomp_out)"
else
    step "Recompiling the game to C (for the $ROM ROM)"
    scripts/recompile.sh "$ROM"
    echo "$stamp" > "$STAMP"
fi

# --- English text, from the disc -----------------------------------------
step "Building the English text from the disc"
scripts/make_text_en.sh "$DISC"

# --- the EBOOT -----------------------------------------------------------
step "Building the EBOOT"
"$MAKE" -j"$JOBS"

# --- dist/AFPSP ----------------------------------------------------------
step "Collecting $OUT"
mkdir -p "$OUT"
cp build/psp/EBOOT.PBP "$OUT/EBOOT.PBP"
cp "work/af/baseroms/$ROM/baserom.z64" "$OUT/baserom.z64"
# The English text is inside the EBOOT; loose copies from an older build would only mislead.
rm -f "$OUT/text_en.bin" "$OUT/names_en.bin"
if [ -n "${KCALL_PRX:-}" ]; then
    cp "$KCALL_PRX" "$OUT/kcall.prx"
fi
ls -l "$OUT"
echo
echo "Done. Copy $OUT to ms0:/PSP/GAME/ on the PSP, or into PPSSPP's PSP/GAME/."
if [ ! -f "$OUT/kcall.prx" ]; then
    echo "On a real PSP also put psp-media-engine-custom-core's kcall.prx in that folder"
    echo "(KCALL_PRX=path/to/kcall.prx ./build.sh ... copies it): with it the audio runs on"
    echo "the Media Engine, without it on the main CPU, which costs speed."
fi
