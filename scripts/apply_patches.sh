#!/usr/bin/env bash
# Applies our changes (patches/*.patch, see docs/CORE_PATCHES.md) to the submodules.
# Idempotent: a patch that is already applied is skipped. Run by scripts/build.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

apply() {  # <submodule dir> <patch file>
    local dir="$ROOT/$1" patch="$ROOT/patches/$2"
    if [ ! -e "$dir/.git" ]; then
        echo "error: $1 is missing; run: git submodule update --init" >&2
        exit 1
    fi
    if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
        return  # already applied
    fi
    if ! git -C "$dir" apply --check "$patch" 2>/dev/null; then
        echo "error: patches/$2 doesn't apply to $1 (local changes, or the submodule moved?)" >&2
        exit 1
    fi
    git -C "$dir" apply "$patch"
    echo "applied patches/$2"
}

if [ ! -e "$ROOT/rcheevos/.git" ]; then
    echo "error: rcheevos is missing; run: git submodule update --init" >&2
    exit 1
fi
apply mds_upstream melonDS.patch
apply borealis borealis.patch
