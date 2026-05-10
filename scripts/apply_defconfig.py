#!/usr/bin/env python3
"""Apply a base defconfig plus optional config fragments."""

import argparse
import os
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply a defconfig and optional config fragments"
    )
    parser.add_argument("defconfig", help="Base defconfig path")
    parser.add_argument(
        "fragments",
        nargs="*",
        help="Optional config fragments merged in order after the base defconfig",
    )
    parser.add_argument("--kconfig", required=True, help="Top-level Kconfig path")
    parser.add_argument(
        "--out",
        default=os.environ.get("KCONFIG_CONFIG", ".config"),
        help="Output .config path",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    kconfig_dir = os.path.join(repo_root, "tools", "kconfig")
    sys.path.insert(0, kconfig_dir)

    import kconfiglib  # pylint: disable=import-error

    kconf = kconfiglib.Kconfig(args.kconfig, suppress_traceback=True)
    print(kconf.load_config(args.defconfig))
    kconf.warn_assign_override = False
    kconf.warn_assign_redun = False
    for fragment in args.fragments:
        print(kconf.load_config(fragment, replace=False))
    print(kconf.write_config(args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
