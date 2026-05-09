#!/usr/bin/env python3
"""Generate compile_commands.json from Makefile variables.

Called by `make analyze` with the real CC, CPPFLAGS, CFLAGS so the compilation
database stays in sync with the actual build — no hand-maintained shadow flags.
"""

import argparse
import json
import os
import shlex
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cc", required=True, help="Compiler command (e.g. riscv-none-elf-gcc)"
    )
    ap.add_argument("--cppflags", default="", help="Preprocessor flags string")
    ap.add_argument("--cflags", default="", help="Compiler flags string")
    ap.add_argument("--config-header", required=True, help="Path to config.h")
    ap.add_argument("--debug", default="0", help="Debug level")
    ap.add_argument("--build-dir", default="build", help="Build output directory")
    ap.add_argument("--root", default=".", help="Project root (absolute)")
    ap.add_argument(
        "--clang-target",
        default="riscv64-none-elf",
        help="Clang --target triple for cross-compilation",
    )
    ap.add_argument("-o", "--output", default="compile_commands.json")
    ap.add_argument("sources", nargs="+", help="Source file list")
    args = ap.parse_args()

    cppflags = shlex.split(args.cppflags)
    cflags = shlex.split(args.cflags)
    root = os.path.abspath(args.root)

    # Strip -MMD -MP (dep tracking, irrelevant for analysis)
    cppflags = [f for f in cppflags if f not in ("-MMD", "-MP")]

    entries = []
    for src in args.sources:
        basename = os.path.basename(src)
        # Use clang as the driver with --target so clang-tidy (-p) works.
        # Preserve all real build flags so analysis matches the actual build.
        arguments = (
            ["clang", f"--target={args.clang_target}"]
            + cppflags
            + ["-include", args.config_header]
            + [f"-D__DEBUG__={args.debug}"]
            + [f'-D__BASENAME__="{basename}"']
            + cflags
            + ["-c", src, "-o", f"{args.build_dir}/{src}.o"]
        )
        entries.append(
            {
                "directory": root,
                "file": src,
                "arguments": arguments,
            }
        )

    with open(args.output, "w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
