#!/usr/bin/env bash
# Security checks for Mazu kernel sources.
# Scans arch/, drivers/, include/, kernel/, lib/, and user/ -- the code that
# actually ships in the kernel image. Tests under tests/ and tools/ are
# excluded because they have looser rules (e.g. host harnesses can use libc).
#
# 1. Banned functions  -- unsafe libc calls with safer alternatives.
# 2. Hard-coded secrets -- catch accidental key leaks in source.
# 3. Dangerous preprocessor -- flag disabled hardening options.
#
# This is a coarse gate, not a parser.  AST-aware semantics live in cppcheck
# (`.ci/check-cppcheck.sh`) and clang-tidy (`scripts/analyze.sh`); this script
# stays a fast grep so it can run without the cross toolchain.

set -euo pipefail

failed=0

banned='(^|[^[:alnum:]_])(gets|sprintf|vsprintf|strcpy|stpcpy|strcat|atoi|atol|atoll|atof|mktemp|tmpnam|tempnam)[[:space:]]*\('
secrets='(password|secret|api_key|private_key|token)[[:space:]]*=[[:space:]]*"[^"]+'
dangerous_pp='#[[:space:]]*(undef|define)[[:space:]]+((_FORTIFY_SOURCE[[:space:]]+0)|(__SSP__))'

# Drop line-leading // and /* … */ comment markers from the report.
# Anchored at start so colons inside the original code line don't trigger
# a false drop. Run AGAINST `grep -nE` output, i.e. lines of the form
# `LINENO:CONTENT`, so the leading anchor sits before the line number.
# Anything more semantic (mid-block-comment hits, trailing code on a line
# that begins with /* … */) is the job of cppcheck or clang-tidy.
numbered_comment='^[0-9]+:[[:space:]]*(//|/\*|\*|\*/)'

# report PATTERN FILE LABEL [grep_extra_flags...]
report() {
    local pattern="$1" file="$2" label="$3"
    shift 3
    local hits
    hits=$(grep -nE "$@" "$pattern" "$file" \
        | grep -vE "$numbered_comment" || true)
    if [ -n "$hits" ]; then
        printf '%s in %s:\n%s\n' "$label" "$file" "$hits"
        return 0
    fi
    return 1
}

while IFS= read -r -d '' f; do
    if report "$banned" "$f" "Banned function"; then
        failed=1
    fi
    if report "$secrets" "$f" "Possible hard-coded secret" -i; then
        failed=1
    fi
    if report "$dangerous_pp" "$f" "Dangerous preprocessor directive"; then
        failed=1
    fi
done < <(git ls-files -z -- \
    'arch/**/*.c' 'arch/**/*.h' \
    'drivers/**/*.c' 'drivers/**/*.h' \
    'include/**/*.h' \
    'kernel/**/*.c' 'kernel/**/*.h' \
    'lib/*.c' 'lib/*.h' \
    'user/**/*.c' 'user/**/*.h')

if [ "$failed" -eq 0 ]; then
    echo "Security checks passed."
fi

exit "$failed"
