#!/usr/bin/env bash
# Verify clang-format-22 conformance for all tracked C/H files.
#
# clang-format style output drifts between major versions; pinning to 22
# keeps CI deterministic regardless of distro defaults.

set -euo pipefail

if [ -z "${CLANG_FORMAT:-}" ]; then
    if command -v clang-format-22 >/dev/null 2>&1; then
        CLANG_FORMAT="clang-format-22"
    elif command -v clang-format >/dev/null 2>&1 \
        && clang-format --version 2>/dev/null | grep -qE 'version 22\.'; then
        CLANG_FORMAT="clang-format"
    else
        echo "Error: clang-format-22 is required (older versions differ in style)" >&2
        exit 1
    fi
fi

# Final guard: the resolved binary must report major version 22.
if ! "$CLANG_FORMAT" --version 2>/dev/null | grep -qE 'version 22\.'; then
    echo "Error: $CLANG_FORMAT is not version 22.x" >&2
    "$CLANG_FORMAT" --version >&2 || true
    exit 1
fi

ret=0
while IFS= read -r -d '' file; do
    expected=$(mktemp)
    if ! "$CLANG_FORMAT" "$file" >"$expected" 2>/dev/null; then
        echo "Error: $CLANG_FORMAT failed on $file" >&2
        rm -f "$expected"
        exit 1
    fi
    if ! diff -u -p --label="$file" --label="expected coding style" "$file" "$expected"; then
        ret=1
    fi
    rm -f "$expected"
done < <(git ls-files -z -- '*.c' '*.h')

exit $ret
