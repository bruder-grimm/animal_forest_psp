#!/usr/bin/env bash
# Regenerates work/recomp_out from the decomp ELF with the PSP fork of N64Recomp.
# Needs: work/af built (scripts/build_n64.sh) and work/N64Recomp built with
# recomp/n64recomp-psp.patch applied (scripts/setup_recomp.sh).
#
#   scripts/recompile.sh [en|jp]
#
# The generated code is specific to the ROM the game plays: en (the default)
# is for the English fan translation, which differs from the Japanese ROM in
# five code bytes (recomp/af.jp.toml); jp is for the Japanese ROM itself. The
# choice is left in work/recomp_out/rom.txt, which the Makefile follows.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECOMP="$ROOT/work/N64Recomp/build/N64Recomp"
LOG="$ROOT/logs/recompile.log"
ROM="${1:-en}"
case "$ROM" in en|jp) ;; *) echo "usage: $0 [en|jp]"; exit 1 ;; esac

[ -x "$RECOMP" ] || { echo "N64Recomp not built; run scripts/setup_recomp.sh"; exit 1; }
[ -f "$ROOT/work/af/build/animalforest-jp.elf" ] || { echo "Decomp ELF missing; run scripts/build_n64.sh"; exit 1; }

mkdir -p "$ROOT/logs"
rm -rf "$ROOT/work/recomp_out"
cd "$ROOT/recomp"
CONFIG=af.jp.toml
if [ "$ROM" = jp ]; then
    # Without the fan translation's code changes. Paths in the file are
    # relative to it, so it has to sit next to the original.
    CONFIG=af.jp.vanilla.toml
    trap 'rm -f "$ROOT/recomp/$CONFIG"' EXIT
    awk '/^# \[fan-translation-begin\]/ { skip = 1 } !skip { print } /^# \[fan-translation-end\]/ { skip = 0 }' \
        af.jp.toml > "$CONFIG"
fi
"$RECOMP" "$CONFIG" > "$LOG" 2>&1
grep -E "Function count|Stubbed|failed to recompile|No overlay relocation" "$LOG" || true

# Give every recompiled function a preemption point. Game code otherwise only
# reaches the runtime at OS calls and indirect calls, and loading a village
# takes it through ~800 ms with one indirect call in it -- long enough to
# starve the game's own audio manager of fifty retraces. See preempt.c.
HOOKED=$(perl -pi -e \
  's/^(RECOMP_FUNC void \w+\(uint8_t\* rdram, recomp_context\* RECOMP_RESTRICT ctx\) \{)$/$1 RECOMP_PREEMPT();/ and $n++;
   END { print STDERR "$n\n" }' \
  "$ROOT/work/recomp_out"/funcs_*.c 2>&1)
echo "Preemption points added to $HOOKED functions"

echo "$ROM" > "$ROOT/work/recomp_out/rom.txt"
echo "Output: $ROOT/work/recomp_out for the $ROM ROM (log: $LOG)"
