#!/usr/bin/env python3
"""Symbolize a clice crash log against a released symbol file.

Release binaries are stripped PIE executables, so the frame addresses in a
crash log are ASLR-shifted. The crash handler records the executable's load
address ("main executable base: 0x..."); this script subtracts it and feeds
the resulting file offsets to llvm-symbolizer.

Usage:
    python symbolize.py crash.log --symbols clice.gsym

Accepts either the released GSYM symbol file (resolved with llvm-gsymutil) or
a full DWARF file / unstripped binary (resolved with llvm-symbolizer). For a
macOS dSYM pass the inner DWARF file (clice.dSYM/Contents/Resources/DWARF/clice),
not the bundle directory. GSYM output keeps names mangled; pipe through
llvm-cxxfilt to demangle.
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
        "--symbols",
        required=True,
        help="symbol file (clice.gsym / clice.debug / dSYM inner DWARF)",
    )
    parser.add_argument(
        "--module",
        default="clice",
        help="frame module name treated as the main executable (default: clice)",
    )
    parser.add_argument("--symbolizer", default="llvm-symbolizer")
    parser.add_argument("--gsymutil", default="llvm-gsymutil")
    args = parser.parse_args()

    tool = args.gsymutil if args.symbols.endswith(".gsym") else args.symbolizer
    if shutil.which(tool) is None:
        print(f"error: {tool} not found in PATH", file=sys.stderr)
        return 1

    # Fail up front rather than silently printing every frame unresolved.
    if not os.path.isfile(args.symbols):
        print(f"error: symbol file not found: {args.symbols}", file=sys.stderr)
        return 1

    if args.log == "-":
        text = sys.stdin.read()
    else:
        with open(args.log) as log_file:
            text = log_file.read()

    if BASE.search(text) is None:
        print(
            "error: no 'main executable base' line in the log; the crash predates "
            "base recording — addresses cannot be rebased",
            file=sys.stderr,
        )
        return 1

    # A log can hold several crash sections appended by respawned processes,
    # each with its own ASLR base — track the most recent one while scanning.
    base = None
    for line in text.splitlines():
        base_match = BASE.search(line)
        if base_match is not None:
            base = int(base_match.group(1), 16)
            print(line)
            continue
        frame = FRAME.match(line)
        if (
            frame is None
            or base is None
            or os.path.basename(frame.group(2)) != args.module
        ):
            print(line)
            continue
        offset = int(frame.group(3), 16) - base
        if offset < 0:
            print(line)
            continue
        if tool == args.gsymutil:
            command = [tool, "--address", hex(offset), args.symbols]
        else:
            command = [tool, f"--obj={args.symbols}", "-f", "-C", "-p", hex(offset)]
        result = subprocess.run(command, capture_output=True, text=True)
        lines = [
            entry
            for entry in result.stdout.splitlines()
            if entry.strip() and "Looking up" not in entry
        ]
        if result.returncode != 0 or not lines:
            print(line)
            continue
        print(f"#{frame.group(1)} {hex(offset)} " + "\n    ".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
