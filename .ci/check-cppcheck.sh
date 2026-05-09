#!/usr/bin/env bash
# Run cppcheck static analysis on Mazu kernel sources.
# Scans arch/, drivers/, kernel/, lib/, and user/ .c files; tests/ and
# tools/ are excluded.
#
# CI mode: --enable=warning + --max-configs=1 for speed; full clang-tidy
# pass lives in scripts/analyze.sh and runs in the analyze workflow.

set -euo pipefail

# Keep the file list NUL-delimited end-to-end so pathnames containing
# embedded newlines or whitespace survive intact. syscall-nr.c style data
# tables can hang older cppcheck versions; the pre-commit hook checks them
# per-file, so skip here too if present.
SOURCES=()
while IFS= read -r -d '' f; do
    case "$f" in
        */syscall-nr.c) continue ;;
    esac
    SOURCES+=("$f")
done < <(git ls-files -z -- \
    'arch/**/*.c' \
    'drivers/**/*.c' \
    'kernel/**/*.c' \
    'lib/*.c' \
    'user/**/*.c')

if [ "${#SOURCES[@]}" -eq 0 ]; then
    echo "No tracked C source files found."
    exit 0
fi

SUPPRESS_OPT=()
if [ -f .cppcheck-suppress ]; then
    SUPPRESS_OPT+=(--suppressions-list=.cppcheck-suppress)
fi

# 120s ceiling guards against runaway analyses on CI runners.
timeout 120 cppcheck \
    -Iinclude -Iarch/riscv64/include -I. \
    --platform=unix64 \
    --enable=warning \
    --max-configs=1 --error-exitcode=1 --inline-suppr \
    --suppress=checkersReport --suppress=unmatchedSuppression \
    --suppress=missingIncludeSystem --suppress=noValidConfiguration \
    --suppress=normalCheckLevelMaxBranches \
    --suppress=preprocessorErrorDirective \
    "${SUPPRESS_OPT[@]}" \
    "${SOURCES[@]}"
