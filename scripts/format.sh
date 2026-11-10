#!/usr/bin/env bash
# Formats all C/C++ sources with the repository's .clang-format. `--check` only reports.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"
mode=(-i)
if [ "${1:-}" = "--check" ]; then mode=(--dry-run --Werror); fi
find include src tests tools adapters \( -name '*.h' -o -name '*.cpp' -o -name '*.c' \) -print0 \
    | xargs -0 clang-format "${mode[@]}"
