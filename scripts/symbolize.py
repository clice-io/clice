#!/usr/bin/env python3
"""Symbolize a clice crash log against a released symbol file.

Release binaries are stripped PIE executables, so the frame addresses in a
crash log are ASLR-shifted. The crash handler records the executable's load
address ("main executable base: 0x..."); this script subtracts it and feeds
the resulting file offsets to llvm-symbolizer.

Usage:
    python symbolize.py crash.log --symbols clice.debug
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# "0  clice 0x00005ea84c157818" — the un-symbolized dump written when the
# crashing machine has no llvm-symbolizer. Already-symbolized "#0 0x..."
# frames do not match and pass through untouched.
FRAME = re.compile(r"^\s*(\d+)\s+(\S+)\s+0x([0-9a-fA-F]+)")
BASE = re.compile(r"main executable base: 0x([0-9a-fA-F]+)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="crash log file (or - for stdin)")
    parser.add_argument(
        "--symbols", required=True, help="symbol file (clice.debug / clice.dSYM)"
    )
    parser.add_argument(
        "--module",
        default="clice",
        help="frame module name treated as the main executable (default: clice)",
    )
    parser.add_argument("--symbolizer", default="llvm-symbolizer")
    args = parser.parse_args()

    if shutil.which(args.symbolizer) is None:
        print(f"error: {args.symbolizer} not found in PATH", file=sys.stderr)
        return 1

    if args.log == "-":
        text = sys.stdin.read()
    else:
        with open(args.log) as log_file:
            text = log_file.read()

    base_match = BASE.search(text)
    if base_match is None:
        print(
            "error: no 'main executable base' line in the log; the crash predates "
            "base recording — addresses cannot be rebased",
            file=sys.stderr,
        )
        return 1
    base = int(base_match.group(1), 16)

    for line in text.splitlines():
        frame = FRAME.match(line)
        if frame is None or os.path.basename(frame.group(2)) != args.module:
            print(line)
            continue
        offset = int(frame.group(3), 16) - base
        if offset < 0:
            print(line)
            continue
        result = subprocess.run(
            [args.symbolizer, f"--obj={args.symbols}", "-f", "-C", "-p", hex(offset)],
            capture_output=True,
            text=True,
        )
        symbolized = result.stdout.strip()
        if result.returncode != 0 or not symbolized:
            print(line)
            continue
        print(f"#{frame.group(1)} {hex(offset)} {symbolized}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
