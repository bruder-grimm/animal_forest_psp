#!/usr/bin/env bash
# Clones N64Recomp at the pinned commit, applies the PSP patch and builds it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/work/N64Recomp"
BASE="$(cat "$ROOT/recomp/n64recomp-base-commit.txt")"

if [ ! -d "$DEST" ]; then
    git clone --recursive https://github.com/N64Recomp/N64Recomp.git "$DEST"
fi
cd "$DEST"
if ! git diff --quiet; then
    echo "N64Recomp has local changes; assuming the PSP patch is already applied."
else
    git checkout -q "$BASE"
    git submodule update --init --recursive -q
    git apply "$ROOT/recomp/n64recomp-psp.patch"
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build --target N64Recomp
echo "Built $DEST/build/N64Recomp"
