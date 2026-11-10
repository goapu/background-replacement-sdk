#!/usr/bin/env bash
# Full verification matrix. Everything the project claims is checked here, from a clean build
# directory, so a green run means the same thing on a laptop and in CI.
#
#   scripts/ci.sh [build-root]
#
# Stages (a stage is skipped, and reported as skipped, when its compiler is not installed):
#   format                       clang-format --dry-run --Werror over all C/C++ sources
#   release-gcc, release-clang   Release, -Werror, full test suite + quality gate
#   static-analysis              clang-tidy (repo .clang-tidy) over src/, warnings are errors
#   asan-ubsan                   Debug with AddressSanitizer and UndefinedBehaviorSanitizer
#   tsan                         Debug with ThreadSanitizer
#   shared                       shared library: C-interface-only exports + tests
#   reproducible                 two Release builds in different directories are byte-identical
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${1:-${root}/build/ci}"
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
generator=()
if command -v ninja >/dev/null 2>&1; then generator=(-G Ninja); fi

passed=()
skipped=()

banner() { printf '\n=== %s ===\n' "$1"; }

# configure_build_test <name> <cxx> <c> <build-type> [cmake args...]
configure_build_test() {
    local name="$1" cxx="$2" cc="$3" type="$4"
    shift 4
    local dir="${build_root}/${name}"
    banner "${name}"
    rm -rf "${dir}"
    cmake -S "${root}" -B "${dir}" "${generator[@]}" \
        -DCMAKE_BUILD_TYPE="${type}" -DCMAKE_CXX_COMPILER="${cxx}" -DCMAKE_C_COMPILER="${cc}" \
        -DMAGIC_PORTAL_WARNINGS_AS_ERRORS=ON "$@"
    cmake --build "${dir}" --parallel "${jobs}"
    (cd "${dir}" && ctest --output-on-failure --parallel "${jobs}")
}

have() { command -v "$1" >/dev/null 2>&1; }

# --- Formatting ---------------------------------------------------------------------------
banner "format"
if have clang-format; then
    bash "${root}/scripts/format.sh" --check
    passed+=("format")
else
    skipped+=("format (clang-format not installed)")
fi

# --- Release with each available compiler -------------------------------------------------
for pair in "gcc:g++:gcc" "clang:clang++:clang"; do
    IFS=: read -r label cxx cc <<<"${pair}"
    if have "${cxx}" && have "${cc}"; then
        compiler_version="$("${cxx}" --version)"
        if [[ "${label}" == gcc && "${compiler_version}" == *[Cc]lang* ]]; then
            skipped+=("release-gcc (${cxx} is a Clang alias, not GCC)")
            continue
        fi
        configure_build_test "release-${label}" "${cxx}" "${cc}" Release
        "${build_root}/release-${label}/magic_portal_eval" --gate
        passed+=("release-${label}")
    else
        skipped+=("release-${label} (${cxx} not installed)")
    fi
done

# --- Static analysis ----------------------------------------------------------------------
banner "static-analysis"
if have clang-tidy; then
    tidy_dir="${build_root}/static-analysis"
    rm -rf "${tidy_dir}"
    cmake -S "${root}" -B "${tidy_dir}" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
    find "${root}/src" -name '*.cpp' -print0 \
        | xargs -0 -n1 -P "${jobs}" clang-tidy -p "${tidy_dir}" --quiet --warnings-as-errors='*'
    passed+=("static-analysis")
else
    skipped+=("static-analysis (clang-tidy not installed)")
fi

default_cxx="$(command -v c++ || command -v g++ || command -v clang++)"
default_cc="$(command -v cc || command -v gcc || command -v clang)"

# --- Sanitizers ---------------------------------------------------------------------------
configure_build_test asan-ubsan "${default_cxx}" "${default_cc}" Debug -DMAGIC_PORTAL_SANITIZE="address;undefined"
passed+=("asan-ubsan")
configure_build_test tsan "${default_cxx}" "${default_cc}" Debug -DMAGIC_PORTAL_SANITIZE="thread"
passed+=("tsan")

# --- Shared library: only the C interface may be exported ----------------------------------
configure_build_test shared "${default_cxx}" "${default_cc}" Release -DMAGIC_PORTAL_BUILD_SHARED=ON
if have nm && [ "$(uname -s)" = "Linux" ]; then
    extra="$(nm -D --defined-only "${build_root}/shared/libmagic_portal.so" | awk '{print $3}' | grep -v '^mp_' || true)"
    if [ -n "${extra}" ]; then
        echo "unexpected exported symbols:"; echo "${extra}"; exit 1
    fi
    echo "shared library exports only mp_* symbols"
fi
passed+=("shared")

# --- Build reproducibility ----------------------------------------------------------------
banner "reproducible"
for n in a b/nested; do
    dir="${build_root}/repro-${n}"
    rm -rf "${dir}"
    cmake -S "${root}" -B "${dir}" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER="${default_cxx}" \
        -DMAGIC_PORTAL_BUILD_TESTS=OFF -DMAGIC_PORTAL_BUILD_TOOLS=OFF >/dev/null
    cmake --build "${dir}" --parallel "${jobs}" >/dev/null
done
hash_a="$(cmake -E sha256sum "${build_root}/repro-a/libmagic_portal.a" | cut -d' ' -f1)"
hash_b="$(cmake -E sha256sum "${build_root}/repro-b/nested/libmagic_portal.a" | cut -d' ' -f1)"
if [ "${hash_a}" != "${hash_b}" ]; then
    echo "library differs between build directories: ${hash_a} vs ${hash_b}"; exit 1
fi
echo "identical libmagic_portal.a: ${hash_a}"
passed+=("reproducible")

banner "summary"
printf 'passed : %s\n' "${passed[*]}"
if [ "${#skipped[@]}" -gt 0 ]; then printf 'skipped: %s\n' "${skipped[*]}"; fi
