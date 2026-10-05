#!/usr/bin/env python3
"""Symbolize a clice crash log against a release's symbol package.

Release binaries are stripped PIE executables, so the frame addresses in a
crash log are ASLR-shifted. The crash handler records the executable's load
address ("main executable base: 0x..."); this script subtracts it and feeds
the resulting file offsets to llvm-symbolizer.

Usage:
    python symbolize.py crash.log --symbols clice

Windows frames name the module, its load address and the offset in it
("0x..., C:\\...\\clice.exe(0x...) + 0x... byte(s)"); the offset is added to
the executable's preferred image base (--image-base), which the addresses in
its symbols count from.

Accepts the unstripped binary of the symbol package, or any full DWARF file
(resolved with llvm-symbolizer), or the GSYM file older releases shipped
(resolved with llvm-gsymutil). The macOS binary names functions only: its
line tables stay in the object files of the build. For a macOS dSYM pass the
inner DWARF file (clice.dSYM/Contents/Resources/DWARF/clice), not the bundle
directory. GSYM output keeps names mangled; pipe through llvm-cxxfilt to
demangle.
"""

import argparse
import ntpath
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
# "0x00007FF6A1B2C3D4, C:\...\clice.exe(0x00007FF6A1B20000) + 0x2C3D4 byte(s)" —
# LLVM's Windows trace when the crashing machine has no llvm-symbolizer.
PE_FRAME = re.compile(
    r"^\s*0x[0-9a-fA-F]+, (.+?)\(0x[0-9a-fA-F]+\) \+ 0x([0-9a-fA-F]+) byte\(s\)",
    re.MULTILINE,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="crash log file (or - for stdin)")
    parser.add_argument(
        "--symbols",
        required=True,
        help="symbol file (the unstripped clice / clice.gsym / dSYM inner DWARF)",
    )
    parser.add_argument(
        "--module",
        default="clice",
        help="frame module name treated as the main executable (default: clice)",
    )
    parser.add_argument(
        "--image-base",
        type=lambda value: int(value, 0),
        default=0x140000000,
        help="preferred image base of a Windows executable (default: lld's for 64-bit EXEs)",
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

    if BASE.search(text) is None and PE_FRAME.search(text) is None:
        print(
            "error: no 'main executable base' line in the log; the crash predates "
            "base recording — addresses cannot be rebased",
            file=sys.stderr,
        )
        return 1

    # A log can hold several crash sections appended by respawned processes,
    # each with its own ASLR base — track the most recent one while scanning.
    base = None
    # Windows frames carry no number; they are counted per crash section.
    pe_frames = 0
    for line in text.splitlines():
        base_match = BASE.search(line)
        if base_match is not None:
            base = int(base_match.group(1), 16)
            pe_frames = 0
            print(line)
            continue
        pe_frame = PE_FRAME.match(line)
        if pe_frame is not None:
            number = str(pe_frames)
            pe_frames += 1
            name = ntpath.basename(pe_frame.group(1)).lower().removesuffix(".exe")
            if name != args.module.lower():
                print(line)
                continue
            offset = args.image_base + int(pe_frame.group(2), 16)
        else:
            frame = FRAME.match(line)
            if (
                frame is None
                or base is None
                or os.path.basename(frame.group(2)) != args.module
            ):
                print(line)
                continue
            number = frame.group(1)
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
        print(f"#{number} {hex(offset)} " + "\n    ".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
