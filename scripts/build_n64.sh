#!/usr/bin/env bash
# Vanilla N64 build of work/af (zeldaret/af, cloned at the commit in
# recomp/af-decomp-commit.txt if it is not there yet), with the macOS
# workarounds it needs. Needs the Japanese ROM in
# work/af/baseroms/jp/baserom.z64 (scripts/prepare_rom.py).
# The port recompiles the ELF this produces (scripts/recompile.sh).
# A src/port left in work/af by the old oot-PSP graft breaks AF's N64 Makefile
# (it globs all of src/), so if there is one it is moved aside for the build
# and always restored.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AF="$ROOT/work/af"
LOG="$ROOT/logs/n64_build.log"
MAKE="$(command -v gmake || command -v make)"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
MAKE_ARGS=()
if [ "$(uname)" = Darwin ]; then
    # the cross binutils' usual home; the decomp's Makefile asks for `gar` here
    [ -d /opt/cross/bin ] && export PATH="/opt/cross/bin:$PATH"
    command -v gar > /dev/null || MAKE_ARGS+=(AR=mips-linux-gnu-ar)
fi

if [ ! -d "$AF/.git" ]; then
    # baseroms/ may already be there (prepare_rom.py), so no plain `git clone`
    mkdir -p "$AF"
    git -C "$AF" init -q
    git -C "$AF" remote add origin https://github.com/zeldaret/af.git
    git -C "$AF" fetch -q origin
    git -C "$AF" checkout -q "$(cat "$ROOT/recomp/af-decomp-commit.txt")"
    git -C "$AF" submodule update --init --recursive -q
fi
[ -f "$AF/baseroms/jp/baserom.z64" ] || { echo "Missing $AF/baseroms/jp/baserom.z64: run scripts/prepare_rom.py <rom>"; exit 1; }

cd "$AF"
restore() { if [ -d "$ROOT/work/_port_aside" ]; then mv "$ROOT/work/_port_aside" "$AF/src/port"; fi; }
trap restore EXIT
if [ -d src/port ]; then
    mv src/port "$ROOT/work/_port_aside"
    rm -rf build/src/port
fi

mkdir -p "$ROOT/logs"
status=0
{
    { [ -d .venv ] || "$MAKE" venv; } &&
    { [ -f build/.setup_done ] || { "$MAKE" setup && "$MAKE" extract && touch build/.setup_done; }; } &&
    "$MAKE" -j"$JOBS" ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"} &&
    "$MAKE" compress ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"}
} > "$LOG" 2>&1 || status=$?
grep -E ": OK|FAILED|Error" "$LOG" | tail -20 || true
echo "full log: $LOG (exit $status)"
exit $status
