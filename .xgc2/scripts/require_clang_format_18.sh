#!/usr/bin/env bash
set -euo pipefail

readonly FORMATTER=clang-format-18
if ! command -v "$FORMATTER" >/dev/null 2>&1; then
  echo "required formatter is unavailable: $FORMATTER" >&2
  exit 1
fi

formatter_path="$(command -v "$FORMATTER")"
formatter_version="$($formatter_path --version)"
if [[ ! "$formatter_version" =~ clang-format[[:space:]]version[[:space:]]18[.] ]]; then
  echo "required formatter major is 18: $formatter_version" >&2
  exit 1
fi

printf '%s\n' "$formatter_path"
