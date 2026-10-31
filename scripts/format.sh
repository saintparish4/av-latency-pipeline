#!/usr/bin/env bash
# Formats every C++/CUDA file git knows about (tracked or new, not ignored)
# with the repository's .clang-format, using the pinned clang-format-18.
#   scripts/format.sh           rewrite files in place
#   scripts/format.sh --check   change nothing; exit 1 if any file would change (CI)
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
mapfile -t files < <(git ls-files -co --exclude-standard -- '*.cpp' '*.hpp' '*.cu')
if [ "${#files[@]}" -eq 0 ]; then
  echo "format: no C++ files yet"
  exit 0
fi
if [ "${1:-}" = "--check" ]; then
  clang-format-18 --dry-run --Werror "${files[@]}"
  echo "format: ${#files[@]} files clean"
else
  changed=()
  for f in "${files[@]}"; do
    clang-format-18 --dry-run --Werror "$f" 2> /dev/null || changed+=("$f")
  done
  if [ "${#changed[@]}" -eq 0 ]; then
    echo "format: all ${#files[@]} files already formatted"
  else
    clang-format-18 -i "${changed[@]}"
    printf '  reformatted %s\n' "${changed[@]}"
    echo "format: reformatted ${#changed[@]} of ${#files[@]} files"
  fi
fi
