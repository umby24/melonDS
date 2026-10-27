#!/usr/bin/env bash
# Build the Switch app inside the official devkitPro AArch64 image (CMake + borealis).
#   scripts/build.sh          -> switch/melonds-next.nro
#   scripts/build.sh clean
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${MELONDS_NEXT_DOCKER_IMAGE:-devkitpro/devkita64:latest}"

if [ "${1:-}" = "clean" ]; then
    rm -rf "$ROOT/switch/build" "$ROOT/switch/melonds-next.nro"
    exit 0
fi

"$ROOT/scripts/apply_patches.sh"

TTY=()
[ -t 1 ] && TTY=(-t)

exec docker run --rm "${TTY[@]}" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$ROOT":/project \
    -w /project \
    "$IMAGE" \
    bash -c "cmake -S switch -B switch/build -DCMAKE_BUILD_TYPE=Release && cmake --build switch/build -j$(nproc) $*"
