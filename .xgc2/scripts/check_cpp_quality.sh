#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/.work/cpp-quality}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --work-dir) WORK_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

for command in cmake ctest; do
  command -v "$command" >/dev/null 2>&1 || {
    echo "missing C++ quality dependency: $command" >&2
    exit 1
  }
done
FORMATTER="$("$SCRIPT_DIR/require_clang_format_18.sh")"
readonly FORMATTER

mapfile -t cpp_files < <(
  find "$REPO_ROOT/include" "$REPO_ROOT/src" "$REPO_ROOT/test" \
    -type f \( \
      -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o \
      -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \
    \) -print | sort
)
(( ${#cpp_files[@]} > 0 )) || {
  echo "C++ quality gate found no source files" >&2
  exit 1
}
"$FORMATTER" --dry-run --Werror "${cpp_files[@]}"

rm -rf "$WORK_DIR"
cmake -S "$REPO_ROOT" -B "$WORK_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/opt/ros/jazzy \
  -DBUILD_TESTING=ON
cmake --build "$WORK_DIR" --parallel "$(nproc)"
ctest --test-dir "$WORK_DIR" --output-on-failure
