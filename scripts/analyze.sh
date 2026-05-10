#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# --- Tool discovery -----------------------------------------------------------

CLANG_BIN="${CLANG_BIN:-clang}"
if ! command -v "$CLANG_BIN" >/dev/null 2>&1; then
    echo "clang not found at '$CLANG_BIN'." >&2
    exit 1
fi

CLANG_TIDY_BIN="${CLANG_TIDY_BIN:-}"
if [ -z "$CLANG_TIDY_BIN" ]; then
    for candidate in \
        /opt/homebrew/opt/llvm/bin/clang-tidy \
        /usr/local/opt/llvm/bin/clang-tidy \
        clang-tidy; do
        if command -v "$candidate" >/dev/null 2>&1; then
            CLANG_TIDY_BIN="$candidate"
            break
        fi
    done
fi
if ! command -v "$CLANG_TIDY_BIN" >/dev/null 2>&1; then
    echo "clang-tidy not found at '$CLANG_TIDY_BIN'." >&2
    exit 1
fi

CPPCHECK_BIN="${CPPCHECK_BIN:-cppcheck}"
HAS_CPPCHECK=0
if command -v "$CPPCHECK_BIN" >/dev/null 2>&1; then
    HAS_CPPCHECK=1
fi

SPARSE_BIN="${SPARSE_BIN:-sparse}"
HAS_SPARSE=0
if command -v "$SPARSE_BIN" >/dev/null 2>&1; then
    HAS_SPARSE=1
fi

# --- Configuration ------------------------------------------------------------

ARCH="${ARCH:-riscv64}"
DEBUG_LEVEL="${DEBUG_LEVEL:-0}"
REPORT_DIR="${REPORT_DIR:-build/analyze}"
COMPDB="${COMPDB:-compile_commands.json}"
JOBS="${JOBS:-1}"

# clang-tidy checks.
# Suppress categories that are false positives in a freestanding kernel:
#   bugprone-reserved-identifier    -- double-underscore include guards (being renamed)
#   bugprone-easily-swappable-parameters -- too noisy for kernel APIs
#   bugprone-suspicious-include     -- selftest .c-in-.c injection pattern
#   bugprone-branch-clone           -- scheduler state-transition switch documents
#                                      each TD_STATE_* case independently; collapsing
#                                      identical empty bodies would erase the inline
#                                      rationale (see sched_finish_task_switch)
#   performance-no-int-to-ptr       -- uptr<->pointer casts fundamental to MMIO/paging
#   clang-analyzer-security.PointerSub -- linker-section iteration (initcall, selftest)
#   clang-analyzer-security.ArrayBound -- false positives on (a) per-CPU arrays
#                                      sized to MAX_CPUS=1 in UP builds where the
#                                      analyzer can't prove cpu==0, and (b)
#                                      container_of / list_for_each_entry pointer
#                                      arithmetic the analyzer can't model
#   clang-analyzer-core.UndefinedBinaryOperatorResult -- byte_view / str_buf
#                                      invariants (dat non-null whenever len>0) are
#                                      structural; the analyzer treats the backing
#                                      pointer as possibly-uninitialized in checksum
#                                      and printk loops
#   clang-analyzer-core.CallAndMessage -- result_T<T>{is_error,value} pattern in
#                                      WebSocket frame parsing: the analyzer can't
#                                      prove frame.payload is filled when
#                                      res.is_error == false, so calls into
#                                      ws_frame_send() with the recv'd pointer
#                                      look like they pass an uninitialized value
#   clang-analyzer-optin.performance.Padding -- struct layouts (e.g. tcp_conn) are
#                                      hand-tuned for cache-line locality and RT
#                                      determinism; reordering for size would
#                                      regress the hot path
CHECKS="${CHECKS:-clang-analyzer-*,bugprone-*,performance-*,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier,-bugprone-suspicious-include,-bugprone-branch-clone,-performance-no-int-to-ptr,-clang-analyzer-security.PointerSub,-clang-analyzer-security.ArrayBound,-clang-analyzer-core.UndefinedBinaryOperatorResult,-clang-analyzer-core.CallAndMessage,-clang-analyzer-optin.performance.Padding,-clang-diagnostic-keyword-macro,-clang-diagnostic-gnu-zero-variadic-macro-arguments,-clang-diagnostic-language-extension-token}"

# Report header diagnostics from project headers only (not system headers)
HEADER_FILTER="${HEADER_FILTER:-'(include/mazu|arch/riscv64/include|drivers|kernel|lib|user)/.*'}"

if [ ! -f build/config.h ]; then
    echo "Missing build/config.h. Run 'make' or 'make defconfig && make' first." >&2
    exit 1
fi

if [ ! -f "$COMPDB" ]; then
    echo "Missing $COMPDB. Run 'make compile_commands.json' first." >&2
    exit 1
fi

mkdir -p "$REPORT_DIR"

# --- File list ----------------------------------------------------------------

if [ "$#" -gt 0 ]; then
    # Explicit files passed on command line
    c_files=()
    for f in "$@"; do
        case "$f" in *.c) c_files+=("$f") ;; esac
    done
else
    # Extract .c file list from compile_commands.json (authoritative source)
    mapfile -t c_files < <(python3 -c "
import json, sys
for e in json.load(open('$COMPDB')):
    print(e['file'])
")
fi

# --- Shared flags for clang --analyze (Phase 1) ------------------------------
# Derived from compile_commands.json so they stay in sync with the real build.
# clang --analyze uses its own compiler driver, so we adapt gcc flags to clang.

read_flags_from_compdb() {
    python3 -c "
import json, sys
db = json.load(open('$COMPDB'))
if not db:
    sys.exit(0)
args = db[0]['arguments']
skip_next = False
out = []
for i, a in enumerate(args):
    if skip_next:
        skip_next = False
        continue
    # Skip the compiler itself, -c, source file, -o, output, -MMD, -MP
    if i == 0:
        continue
    if a in ('-c', '-o', '-MMD', '-MP'):
        if a in ('-c', '-o'):
            skip_next = True
        continue
    # Skip per-file __BASENAME__
    if '__BASENAME__' in a:
        continue
    out.append(a)
print(' '.join(out))
"
}

compdb_flags="$(read_flags_from_compdb)"
# Replace gcc target triple with clang-compatible target
clang_analyze_flags=(-target riscv64-none-elf)
# shellcheck disable=SC2086
for flag in $compdb_flags; do
    case "$flag" in
        -march=* | -mabi=* | -mcmodel=* | -mno-relax) clang_analyze_flags+=("$flag") ;;
        -I* | -include | -D* | -std=* | -f* | -nostdinc | -Wall | -Wextra | -Wuninitialized | -pedantic)
            clang_analyze_flags+=("$flag")
            ;;
        # Carry -include's argument
        build/config.h) clang_analyze_flags+=("$flag") ;;
    esac
done

# --- Phase 1: clang static analyzer ------------------------------------------

ANALYZE_LOG="$REPORT_DIR/analyze.log"
: >"$ANALYZE_LOG"

echo "== Phase 1: clang static analyzer =="
rc=0
a_warnings=0
a_errors=0

# Disabled checkers for the standalone analyzer (Phase 1).  Note that
# -Xanalyzer -analyzer-disable-checker errors hard on an unknown checker
# name, so this list must only contain checkers the deployed clang
# actually registers; checker names not in scope here are still
# suppressed in CHECKS for Phase 2 (clang-tidy ignores unknown disables).
#   core.UndefinedBinaryOperatorResult, core.uninitialized.Assign --
#       byte_view dereference patterns in hand-rolled checksum / printk
#       loops trip the analyzer because it can't prove the (dat,len)
#       invariant of struct byte_view across translation units.
#   core.CallAndMessage -- result_T pattern in WebSocket recv loop;
#       analyzer can't prove frame.payload is initialized when
#       res.is_error is false, so passing it to ws_frame_send() looks
#       like an uninitialized argument.
# security.ArrayBound is intentionally omitted here.  clang 18 (Ubuntu
# 24.04 stock) ships it as alpha.security.ArrayBoundV2 instead, and
# passing the new name to an older driver fails the run; the Phase 1
# pass does not surface ArrayBound on these patterns anyway.  Phase 2
# clang-tidy keeps -clang-analyzer-security.ArrayBound in CHECKS for
# the newer drivers that do fire it via --header-filter.
# Exported so the GNU-parallel branch below can reference it through env
# rather than splicing the list into a doubly-quoted shell command.  The
# ${VAR:-default} form mirrors CLANG_BIN/CLANG_TIDY_BIN/etc. so callers
# can extend the list (e.g. when bringing up a new clang version that
# fires a different false-positive checker on the same patterns).
export ANALYZER_DISABLED_CHECKERS="${ANALYZER_DISABLED_CHECKERS:-core.UndefinedBinaryOperatorResult,core.uninitialized.Assign,core.CallAndMessage}"

run_analyze() {
    local f="$1" tmp
    tmp="$(mktemp "$REPORT_DIR"/analyze.XXXXXX)"
    if ! "$CLANG_BIN" --analyze -Xanalyzer -analyzer-output=text \
        -Xanalyzer -analyzer-disable-checker="$ANALYZER_DISABLED_CHECKERS" \
        "${clang_analyze_flags[@]}" \
        -D__BASENAME__="\"$(basename "$f")\"" "$f" \
        >"$tmp" 2>&1; then
        touch "$REPORT_DIR/.analyze_fail"
    fi
    echo "$tmp"
}

if [ "$JOBS" -gt 1 ] && command -v parallel >/dev/null 2>&1; then
    export CLANG_BIN REPORT_DIR
    export clang_analyze_flags_str="${clang_analyze_flags[*]}"
    # ANALYZER_DISABLED_CHECKERS is referenced through env so an edited
    # list (including any future entry containing whitespace) survives
    # the parallel sub-shell quoting.
    printf '%s\n' "${c_files[@]}" | parallel -j "$JOBS" "
        tmp=\$(mktemp \"$REPORT_DIR\"/analyze.XXXXXX)
        $CLANG_BIN --analyze -Xanalyzer -analyzer-output=text \
            -Xanalyzer -analyzer-disable-checker=\"\$ANALYZER_DISABLED_CHECKERS\" \
            ${clang_analyze_flags[*]} \
            -D__BASENAME__=\\\"\$(basename {})\\\" {} >\"\$tmp\" 2>&1 || \
            touch \"$REPORT_DIR/.analyze_fail\"
        echo \"  ANALYZE {}\" >&2
        cat \"\$tmp\"
        rm -f \"\$tmp\"
    " >>"$ANALYZE_LOG" 2>&1
else
    for f in "${c_files[@]}"; do
        echo "  ANALYZE $f"
        tmp="$(run_analyze "$f")"
        cat "$tmp" | tee -a "$ANALYZE_LOG"
        rm -f "$tmp"
    done
fi

if [ -f "$REPORT_DIR/.analyze_fail" ]; then
    rc=1
    rm -f "$REPORT_DIR/.analyze_fail"
fi
a_warnings=$(grep -c ": warning:" "$ANALYZE_LOG" || true)
a_errors=$(grep -c ": error:" "$ANALYZE_LOG" || true)
echo "  analyzer: ${a_warnings} warning(s), ${a_errors} error(s)"
# Phase 1 only sets rc=1 on tool crashes (.analyze_fail). Without this
# guard, residual warnings accumulate silently while make analyze still
# exits 0, defeating the TODO 9w policy that any non-failing finding
# must be recorded in a suppression list with rationale.
if [ "$a_warnings" -gt 0 ] || [ "$a_errors" -gt 0 ]; then
    rc=1
fi

# --- Phase 2: clang-tidy (uses compile_commands.json) -------------------------

TIDY_LOG="$REPORT_DIR/tidy.log"
: >"$TIDY_LOG"

echo "== Phase 2: clang-tidy =="
t_warnings=0
t_errors=0

# Run clang-tidy on .c files only; --header-filter surfaces header diagnostics
# through their owning translation units (more accurate than standalone .h runs).
for f in "${c_files[@]}"; do
    echo "  TIDY    $f"
    tmp="$(mktemp "$REPORT_DIR"/tidy.XXXXXX)"
    if ! "$CLANG_TIDY_BIN" \
        --checks="$CHECKS" \
        --header-filter="$HEADER_FILTER" \
        -p "$ROOT_DIR" \
        "$f" \
        >"$tmp" 2>&1; then
        rc=1
    fi
    # Filter clang-tidy informational noise (suppression counts, header-filter hints)
    grep -v -E '(^Suppressed |^Use -header-filter|warnings generated\.$)' \
        "$tmp" | tee -a "$TIDY_LOG" || true
    rm -f "$tmp"
done

t_warnings=$(grep -c ": warning:" "$TIDY_LOG" || true)
t_errors=$(grep -c ": error:" "$TIDY_LOG" || true)
echo "  tidy: ${t_warnings} warning(s), ${t_errors} error(s)"
# Phase 2 mirrors the Phase 1 strictness gate: clang-tidy returns 0 by
# default even when warnings are emitted, so without this make analyze
# would silently accumulate noise.
if [ "$t_warnings" -gt 0 ] || [ "$t_errors" -gt 0 ]; then
    rc=1
fi

# --- Phase 3: cppcheck (uses compile_commands.json) ---------------------------

CPPCHECK_LOG="$REPORT_DIR/cppcheck.log"
: >"$CPPCHECK_LOG"
c_warnings=0
c_errors=0
c_style=0
c_perf=0
c_port=0

if [ "$HAS_CPPCHECK" -eq 1 ]; then
    echo "== Phase 3: cppcheck =="
    SUPPRESS_FILE="$ROOT_DIR/.cppcheck-suppress"
    # cppcheck --project= imports flags from compile_commands.json but does not
    # define compiler builtins (__GNUC__, __has_builtin, etc.).  Supply them so
    # headers that guard on compiler identity (#ifndef __GNUC__) work correctly.
    cppcheck_args=(
        --project="$COMPDB"
        --enable=warning,style,performance,portability
        --std=c99
        --suppress=missingIncludeSystem
        -D__GNUC__=12 -D__GNUC_MINOR__=0
        -U__has_attribute -U__has_builtin
        --force
        --template='{file}:{line}: {severity}[{id}]: {message}'
    )
    if [ -f "$SUPPRESS_FILE" ]; then
        cppcheck_args+=("--suppressions-list=$SUPPRESS_FILE")
    fi

    # cppcheck writes findings and progress to stderr.
    tmp_cppcheck="$(mktemp "$REPORT_DIR"/cppcheck.XXXXXX)"
    cppcheck_rc=0
    "$CPPCHECK_BIN" "${cppcheck_args[@]}" 2>"$tmp_cppcheck" || cppcheck_rc=$?

    # Keep only lines matching our template (findings, not progress)
    grep -E '^.+:[0-9]+: (error|warning|style|performance|portability)\[' \
        "$tmp_cppcheck" >"$CPPCHECK_LOG" 2>/dev/null || true

    # Detect tool failures vs analysis findings
    if [ "$cppcheck_rc" -ne 0 ]; then
        if grep -q "cppcheck: error:" "$tmp_cppcheck" 2>/dev/null; then
            echo "cppcheck tool error (exit $cppcheck_rc):" >&2
            grep "cppcheck: error:" "$tmp_cppcheck" >&2
            rc=1
        fi
    fi
    rm -f "$tmp_cppcheck"

    cat "$CPPCHECK_LOG"
    c_warnings=$(grep -c ": warning\[" "$CPPCHECK_LOG" || true)
    c_errors=$(grep -c ": error\[" "$CPPCHECK_LOG" || true)
    c_style=$(grep -c ": style\[" "$CPPCHECK_LOG" || true)
    c_perf=$(grep -c ": performance\[" "$CPPCHECK_LOG" || true)
    c_port=$(grep -c ": portability\[" "$CPPCHECK_LOG" || true)
    if [ "$c_errors" -gt 0 ]; then rc=1; fi
    echo "  cppcheck: ${c_errors} error(s), ${c_warnings} warning(s), ${c_style} style, ${c_perf} performance, ${c_port} portability"
else
    echo "== Phase 3: cppcheck (skipped, not installed) =="
fi

# --- Phase 4: sparse (optional) -----------------------------------------------

SPARSE_LOG="$REPORT_DIR/sparse.log"
: >"$SPARSE_LOG"
s_warnings=0
s_errors=0

if [ "$HAS_SPARSE" -eq 1 ]; then
    echo "== Phase 4: sparse =="
    # sparse uses gcc-compatible flags; feed it the real build flags from compdb.
    # Extract flags from the first entry (shared across all TUs).
    sparse_base_flags="$(python3 -c "
import json
db = json.load(open('$COMPDB'))
if not db:
    exit(0)
args = db[0]['arguments']
skip_next = False
out = []
for i, a in enumerate(args):
    if skip_next:
        skip_next = False
        continue
    if i == 0:
        continue
    if a in ('-c', '-o'):
        skip_next = True
        continue
    if '--target=' in a:
        continue
    if '__BASENAME__' in a:
        continue
    out.append(a)
print(' '.join(out))
")"

    for f in "${c_files[@]}"; do
        echo "  SPARSE  $f"
        tmp="$(mktemp "$REPORT_DIR"/sparse.XXXXXX)"
        # sparse writes diagnostics to stderr
        # shellcheck disable=SC2086
        "$SPARSE_BIN" $sparse_base_flags \
            -D__BASENAME__="\"$(basename "$f")\"" \
            "$f" 2>"$tmp" || true
        cat "$tmp" | tee -a "$SPARSE_LOG"
        rm -f "$tmp"
    done

    s_warnings=$(grep -c ": warning:" "$SPARSE_LOG" || true)
    s_errors=$(grep -c ": error:" "$SPARSE_LOG" || true)
    if [ "$s_errors" -gt 0 ]; then rc=1; fi
    echo "  sparse: ${s_errors} error(s), ${s_warnings} warning(s)"
else
    echo "== Phase 4: sparse (skipped, not installed) =="
fi

# --- Summary ------------------------------------------------------------------

c_total=$((c_errors + c_warnings + c_style + c_perf + c_port))
s_total=$((s_errors + s_warnings))

SUMMARY_FILE="$REPORT_DIR/summary.txt"
{
    echo "files=${#c_files[@]}"
    echo "analyze_warnings=$a_warnings"
    echo "analyze_errors=$a_errors"
    echo "tidy_warnings=$t_warnings"
    echo "tidy_errors=$t_errors"
    echo "cppcheck_errors=$c_errors"
    echo "cppcheck_warnings=$c_warnings"
    echo "cppcheck_style=$c_style"
    echo "cppcheck_performance=$c_perf"
    echo "cppcheck_portability=$c_port"
    echo "sparse_errors=$s_errors"
    echo "sparse_warnings=$s_warnings"
    echo "clang=$CLANG_BIN"
    echo "clang_tidy=$CLANG_TIDY_BIN"
    echo "cppcheck=$([ "$HAS_CPPCHECK" -eq 1 ] && echo "$CPPCHECK_BIN")"
    echo "sparse=$([ "$HAS_SPARSE" -eq 1 ] && echo "$SPARSE_BIN")"
    echo "checks=$CHECKS"
} >"$SUMMARY_FILE"

total=$((a_warnings + a_errors + t_warnings + t_errors + c_total + s_total))
echo "== Summary: ${total} finding(s) (analyzer:$((a_warnings + a_errors)) tidy:$((t_warnings + t_errors)) cppcheck:${c_total} sparse:${s_total}) =="
echo "   Reports: $ANALYZE_LOG, $TIDY_LOG, $CPPCHECK_LOG, $SPARSE_LOG"

exit "$rc"
