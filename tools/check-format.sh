#!/bin/sh
#
# Checks the layout (algotradelib's `.clang-format`, this script copied from algotradelib at d251dd4);
# it only reads, and exits non-zero on any finding.
#
# 1. clang-format: every C++ file matches `.clang-format`.
# 2. Visual alignment: no code line indented more than 2 past the code line above it (`check-format.awk`).
#    clang-format accepts its own output, and its output aligns in a few shapes it cannot avoid
#    (`<<` under `<<`, a literal under a literal, a ternary hung from a `(`), so this is checked apart.
# 3. Padded trailing comments: no `//` after two or more spaces, in code, doc comments or the Markdown docs.
#
# Use the clang-format CI uses (`pipx install clang-format==23.1.3`); another version may format differently.
# To fix a file: `clang-format -i <file>`, or `git clang-format main` for changed lines (it skips the
# extensionless header include/cmdatl/cmdatl).
#
# Usage:
#   tools/check-format.sh
#
# Environment:
#   CLANG_FORMAT  the clang-format to run (default: clang-format)

set -eu

cd "$(dirname "$0")/.."
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
failed=0

cpp_files() {
    git ls-files '*.hpp' '*.cpp'
    git ls-files include | grep -E '^include/[^/]+/[^/.]+$'
}

markdown_files() {
    git ls-files 'README*.md' 'docs/*.md' 'skills/*.md'
}

if ! cpp_files | xargs "$CLANG_FORMAT" --dry-run --Werror; then
    failed=1
fi

if ! cpp_files | xargs awk -f tools/check-format.awk; then
    failed=1
fi

# A line whose first non-blank characters are `//` is skipped: commented-out code keeps its own indentation.
padded=$(
    { cpp_files; markdown_files; } | xargs grep -nE '[^ ] {2,}//' /dev/null | grep -vE '^[^:]+:[0-9]+: *//' || true
)
if [ -n "$padded" ]; then
    printf '%s\n' "$padded" | sed 's/^\([^:]*:[0-9]*\):.*/\1: a trailing comment after more than one space/'
    failed=1
fi

exit "$failed"
