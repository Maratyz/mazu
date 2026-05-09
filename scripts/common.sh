#!/usr/bin/env bash
# Shared utilities for Mazu git hooks and scripts.

# Terminal color support with graceful degradation for CI/headless environments.
set_colors() {
    RED="" GREEN="" YELLOW="" CYAN="" NC=""
    if [ -t 1 ] && command -v tput >/dev/null 2>&1; then
        local ncolors
        ncolors=$(tput colors 2>/dev/null || echo 0)
        if [ "$ncolors" -ge 8 ]; then
            RED=$(tput setaf 1)
            GREEN=$(tput setaf 2)
            YELLOW=$(tput setaf 3)
            CYAN=$(tput setaf 6)
            NC=$(tput sgr0)
        fi
    fi
}
