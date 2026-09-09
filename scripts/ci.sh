#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$(basename "${ROOT}")"
BUILD_DIR="${BUILD_DIR:-/var/tmp/${PROJECT}-ci-build}"
FUZZ_BUILD_DIR="${FUZZ_BUILD_DIR:-/var/tmp/${PROJECT}-ci-fuzz-build}"
COVERAGE_MIN="${COVERAGE_MIN:-80}"
FUZZ_SECONDS="${FUZZ_SECONDS:-30}"
JOBS="${JOBS:-$(nproc)}"
FUZZ_TARGET="${FUZZ_TARGET:-fuzz_worker}"
EXTRA_CMAKE_ARGS=()

export TMPDIR="${TMPDIR:-/var/tmp/${PROJECT}-ci-tmp}"
mkdir -p "${TMPDIR}"
export CCACHE_DISABLE="${CCACHE_DISABLE:-1}"
export CC="${CC:-/usr/bin/gcc}"
export CXX="${CXX:-/usr/bin/g++}"

source_paths() {
    find "${ROOT}/src" "${ROOT}/include" -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | sort
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Required command not found: $1" >&2
        exit 1
    fi
}

cmd_format() {
    require_cmd clang-format
    mapfile -t files < <(source_paths)
    if [[ "${#files[@]}" -eq 0 ]]; then
        echo "No C++ sources to format-check."
        return 0
    fi
    clang-format --dry-run --Werror -style=file "${files[@]}"
}

lint_paths() {
    source_paths | grep -v '/fuzz/' || true
}

cmd_lint() {
    require_cmd clang-tidy
    cmd_configure_debug
    mapfile -t files < <(lint_paths)
    local failed=0
    for file in "${files[@]}"; do
        if ! clang-tidy -p "${BUILD_DIR}" "${file}"; then
            failed=1
        fi
    done
    return "${failed}"
}

cmd_security() {
    require_cmd cppcheck
    mapfile -t files < <(source_paths)
    cppcheck \
        --enable=warning,performance,portability \
        --error-exitcode=1 \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        -I "${ROOT}/include" \
        "${files[@]}"
}

cmd_configure_debug() {
    cmake -B "${BUILD_DIR}" -S "${ROOT}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DTEEZ_ENABLE_COVERAGE=OFF \
        "${EXTRA_CMAKE_ARGS[@]}"
    cmake --build "${BUILD_DIR}" -j "${JOBS}"
}

cmd_configure_coverage() {
    cmake -B "${BUILD_DIR}" -S "${ROOT}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DTEEZ_ENABLE_COVERAGE=ON \
        "${EXTRA_CMAKE_ARGS[@]}"
    cmake --build "${BUILD_DIR}" -j "${JOBS}"
}

cmd_test() {
    cmd_configure_debug
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
}

cmd_coverage() {
    require_cmd gcovr
    cmd_configure_coverage
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
    cmake --build "${BUILD_DIR}" --target coverage

    local -a gcovr_args=(
        --root "${ROOT}"
        --object-directory "${BUILD_DIR}"
        --filter "${ROOT}/src"
        --filter "${ROOT}/include"
        --exclude '.*/tests/.*'
        --exclude '.*/_deps/.*'
        --exclude '.*/build/.*'
        --exclude '.*/CMakeFiles/.*'
        --gcov-ignore-errors=source_not_found
        --fail-under-line "${COVERAGE_MIN}"
        --print-summary
    )
    gcovr "${gcovr_args[@]}"
}

cmd_fuzz() {
    require_cmd clang++
    export CC=clang
    export CXX=clang++
    cmake -B "${FUZZ_BUILD_DIR}" -S "${ROOT}" \
        -DTEEZ_ENABLE_FUZZ=ON \
        -DTEEZ_ENABLE_COVERAGE=OFF \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        "${EXTRA_CMAKE_ARGS[@]}"
    cmake --build "${FUZZ_BUILD_DIR}" --target "${FUZZ_TARGET}"

    local corpus_dir="${ROOT}/fuzz/corpus"
    mkdir -p "${corpus_dir}"
    if [[ -d "${ROOT}/fuzz/seeds" ]]; then
        cp -n "${ROOT}/fuzz/seeds/"* "${corpus_dir}/" 2>/dev/null || true
    fi

    "${FUZZ_BUILD_DIR}/${FUZZ_TARGET}" \
        "${corpus_dir}" \
        "-max_total_time=${FUZZ_SECONDS}" \
        "-rss_limit_mb=2048"
}

cmd_all() {
    cmd_format
    cmd_security
    cmd_lint
    cmd_test
    cmd_coverage
    cmd_fuzz
}

usage() {
    cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  all        Run format, security, lint, test, coverage, and fuzz
  format     Check clang-format
  lint       Run clang-tidy
  security   Run cppcheck
  test       Configure, build, and run ctest
  coverage   Run tests with coverage and enforce >= ${COVERAGE_MIN}% line rate
  fuzz       Build and run libFuzzer target (${FUZZ_TARGET})
EOF
}

main() {
    local command="${1:-all}"
    case "${command}" in
        all) cmd_all ;;
        format) cmd_format ;;
        lint) cmd_lint ;;
        security) cmd_security ;;
        test) cmd_test ;;
        coverage) cmd_coverage ;;
        fuzz) cmd_fuzz ;;
        -h|--help|help) usage ;;
        *)
            echo "Unknown command: ${command}" >&2
            usage >&2
            exit 1
            ;;
    esac
}

main "$@"
