#!/usr/bin/env bash
# Builds the Animal Forest PSP port inside Docker: the only thing to install is
# Docker. Takes the same arguments as build.sh and leaves the same result,
# dist/AFPSP/ (with kcall.prx included).
#
#   ./docker-build.sh [FILE...]
#
# Put your dumps in the roms/ folder (roms/README.md says which): they are found by
# their content, whatever they are called. FILEs named on the command line are
# looked at too, wherever they are.
#
# The first run builds the image (a few minutes: it downloads the PSP toolchain)
# and then everything build.sh does (about ten minutes on 4 cores, even emulated on
# an Apple Silicon Mac); later runs only redo what changed. The decomp, N64Recomp and the
# object files live in two Docker volumes, afpsp-work and afpsp-build, which keeps
# them apart from a work/ and build/ you may have on the host (`docker volume rm
# afpsp-work afpsp-build` starts from scratch). Environment: JOBS (parallel jobs,
# default: all of Docker's cores), AFPSP_IMAGE (image name, default afpsp-build).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${AFPSP_IMAGE:-afpsp-build}"

case "${1:-}" in -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;; esac
command -v docker > /dev/null || { echo "Docker is not installed (https://docs.docker.com/get-docker/)"; exit 1; }
docker info > /dev/null 2>&1 || { echo "Docker is installed but not running"; exit 1; }

# Files named on the command line are mounted read-only under /in, whatever they are
# called or wherever they are; roms/ is already there, in the mounted repository.
mounts=()
inputs=()
n=0
for f in "$@"; do
    [ -f "$f" ] || { echo "No such file: $f"; exit 1; }
    path="$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
    target="/in/$n-$(basename "$f")"
    mounts+=(-v "$path:$target:ro")
    inputs+=("$target")
    n=$((n + 1))
done

docker build --platform linux/amd64 -t "$IMAGE" "$ROOT"

mkdir -p "$ROOT/dist" "$ROOT/logs"
docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    ${JOBS:+-e JOBS="$JOBS"} \
    -v "$ROOT:/src" -v afpsp-work:/src/work -v afpsp-build:/src/build \
    ${mounts[@]+"${mounts[@]}"} \
    "$IMAGE" ${inputs[@]+"${inputs[@]}"}

echo
echo "The result is in $ROOT/dist/AFPSP"
