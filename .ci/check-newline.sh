#!/usr/bin/env bash
# Ensure all tracked C/H files end with a newline.

set -e -u -o pipefail

ret=0
while IFS= read -rd '' f; do
    # --brief suppresses the filename prefix; without it, a path like
    # 'foo/binary_search.c' would match `grep -qv binary` and skip the check.
    if file --brief --mime-encoding "$f" | grep -qv binary; then
        if [ -n "$(tail -c1 < "$f")" ]; then
            echo "Warning: No newline at end of file $f"
            ret=1
        fi
    fi
done < <(git ls-files -z -- '*.c' '*.h')

exit $ret
