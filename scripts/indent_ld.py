#!/usr/bin/env python3
"""Indent GNU ld linker scripts by brace depth.

Usage: indent_ld.py [file ...]

Re-indents each file in place using 4-space indentation derived from
brace nesting depth.  Block comments (/* ... */) are preserved with
their interior lines indented one extra level relative to the opening
slash-star.  Blank lines are kept as-is.
"""

import re
import sys

INDENT = "    "

BLOCK_OPEN = re.compile(r"/\*")
BLOCK_CLOSE = re.compile(r"\*/")


def _count(line, ch):
    """Count unquoted occurrences of *ch* outside comments."""
    return line.count(ch)


def indent_file(path):
    with open(path) as f:
        lines = f.readlines()

    depth = 0
    in_block_comment = False
    comment_depth = 0
    out = []

    for raw in lines:
        stripped = raw.strip()

        # --- blank lines pass through unchanged ---
        if not stripped:
            out.append("\n")
            continue

        # --- block-comment tracking ---
        if in_block_comment:
            # Interior or closing line of a block comment.
            if BLOCK_CLOSE.search(stripped):
                out.append(INDENT * comment_depth + " " + stripped + "\n")
                in_block_comment = False
            else:
                out.append(INDENT * comment_depth + " " + stripped + "\n")
            continue

        # Check if this line opens a block comment that does NOT close on
        # the same line (i.e. a multi-line comment).
        if BLOCK_OPEN.search(stripped) and not BLOCK_CLOSE.search(stripped):
            comment_depth = depth
            out.append(INDENT * depth + stripped + "\n")
            in_block_comment = True
            continue

        # --- brace-depth adjustment ---
        opens = _count(stripped, "{")
        closes = _count(stripped, "}")

        # Lines starting with '}' print at reduced depth.
        if stripped.startswith("}"):
            depth = max(depth - 1, 0)
            out.append(INDENT * depth + stripped + "\n")
            # Account for any *additional* close braces beyond the first,
            # and any opens on the same line (e.g. '} .foo : {').
            depth = max(depth + opens - (closes - 1), 0)
        else:
            out.append(INDENT * depth + stripped + "\n")
            depth = max(depth + opens - closes, 0)

    with open(path, "w") as f:
        f.writelines(out)


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} file [file ...]", file=sys.stderr)
        sys.exit(1)
    for path in sys.argv[1:]:
        indent_file(path)


if __name__ == "__main__":
    main()
