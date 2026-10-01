#!/usr/bin/env bash
# Regenerates work/recomp_out from the decomp ELF with the PSP fork of N64Recomp.
# Needs: work/af built (scripts/build_n64.sh) and work/N64Recomp built with
# recomp/n64recomp-psp.patch applied (scripts/setup_recomp.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECOMP="$ROOT/work/N64Recomp/build/N64Recomp"
LOG="$ROOT/logs/recompile.log"

[ -x "$RECOMP" ] || { echo "N64Recomp not built; run scripts/setup_recomp.sh"; exit 1; }
[ -f "$ROOT/work/af/build/animalforest-jp.elf" ] || { echo "Decomp ELF missing; run scripts/build_n64.sh"; exit 1; }

mkdir -p "$ROOT/logs"
rm -rf "$ROOT/work/recomp_out"
cd "$ROOT/recomp"
"$RECOMP" af.jp.toml > "$LOG" 2>&1
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

echo "Output: $ROOT/work/recomp_out (log: $LOG)"
