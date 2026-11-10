#!/usr/bin/env bash
# Repository hygiene checks. Fails if the tree contains machine-specific paths, oversized
# files or media that should not be committed.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"
status=0

# Tracked-looking files only (skip build output and VCS metadata).
files() { find . -type f -not -path './build/*' -not -path './.git/*' -print0; }

echo "-- absolute user-home paths"
if files | xargs -0 grep -I -n -E '(/Users/[A-Za-z0-9_.-]+/|/home/[A-Za-z0-9_.-]+/|C:\\Users\\)' -- 2>/dev/null; then
    echo "found machine-specific absolute paths"; status=1
fi

echo "-- files larger than 1 MiB"
if [ -n "$(find . -type f -size +1024k -not -path './build/*' -not -path './.git/*')" ]; then
    find . -type f -size +1024k -not -path './build/*' -not -path './.git/*'
    echo "large files must not be committed"; status=1
fi

echo "-- image / video / model files"
if find . -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.mp4' \
        -o -iname '*.mov' -o -iname '*.tflite' \) -not -path './build/*' -not -path './.git/*' | grep .; then
    echo "media and models are kept out of the repository"; status=1
fi

[ "${status}" -eq 0 ] && echo "hygiene ok"
exit "${status}"
