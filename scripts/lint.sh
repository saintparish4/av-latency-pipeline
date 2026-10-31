#!/usr/bin/env bash
# Static checks the compiler does not do:
#   C++    clang-tidy-18 with .clang-tidy, over every source in build/rel's
#          compile database (so configure the rel preset first)
#   shell  shellcheck (defects) and shfmt (layout: 2-space indent, `> file`)
# Usage:
#   scripts/lint.sh          report; exit 1 on any finding
#   scripts/lint.sh --quick  skip clang-tidy (seconds instead of a minute)
#   scripts/lint.sh --fix    rewrite shell layout with shfmt, then report
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
MODE=${1:-}
SHFMT=(shfmt -i 2 -ci -sr)
status=0

mapfile -t scripts < <(git ls-files -co --exclude-standard -- '*.sh')
if [ "${#scripts[@]}" -gt 0 ]; then
  [ "$MODE" = "--fix" ] && "${SHFMT[@]}" -w "${scripts[@]}"
  shellcheck "${scripts[@]}" || status=1
  "${SHFMT[@]}" -d "${scripts[@]}" || status=1
fi

if [ "$MODE" != "--quick" ]; then
  if [ ! -f build/rel/compile_commands.json ]; then
    echo "lint: no build/rel/compile_commands.json; run: cmake --preset rel"
    exit 1
  fi
  mkdir -p build
  if ! run-clang-tidy-18 -p build/rel -quiet "$PWD/(src|bench|tools|tests)/" > build/clang-tidy.log 2>&1; then
    grep -A3 -E "(warning|error):" build/clang-tidy.log || cat build/clang-tidy.log
    status=1
  fi
fi

[ "$status" -eq 0 ] && echo "lint: clean"
exit "$status"
