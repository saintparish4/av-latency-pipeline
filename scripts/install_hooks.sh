#!/usr/bin/env bash
# Installs a git pre-commit hook that refuses a commit whose C++ is not
# clang-formatted or whose scripts fail shellcheck/shfmt. clang-tidy is left
# to scripts/lint.sh and CI: at about a minute it is too slow for every commit.
# Hooks live in .git/, which is not versioned, so run this once per clone.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
hook=.git/hooks/pre-commit
cat > "$hook" << 'HOOK'
#!/usr/bin/env bash
# Installed by scripts/install_hooks.sh. Bypass once with: git commit --no-verify
set -e
scripts/format.sh --check
scripts/lint.sh --quick
HOOK
chmod +x "$hook"
echo "installed $hook"
