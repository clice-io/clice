#!/usr/bin/env python3
"""Symbolize a clice crash log against a released symbol file.

Each frame of a crash names its module, where the module is loaded and the
offset in it ("0x..., /usr/bin/clice(0x...) + 0x... byte(s)"): the offset is
the address the symbols of an ELF or Mach-O file count, and for a Windows
module (.exe, .dll) the address from the executable's preferred image base
(--image-base). Every frame but the one a signal or an exception interrupted
(its line ends with " (interrupted)") is a return address, whose call is the
byte before it. Older logs number their frames and record the executable's
load address ("main executable base: 0x...") instead, which this script
subtracts.

Usage:
    python symbolize.py crash.log --symbols clice.gsym

Accepts either the released GSYM symbol file (resolved with llvm-gsymutil) or
a full DWARF file / unstripped binary (resolved with llvm-symbolizer). For a
macOS dSYM pass the inner DWARF file (clice.dSYM/Contents/Resources/DWARF/clice),
not the bundle directory. GSYM output keeps names mangled; pipe through
llvm-cxxfilt to demangle.
"""

import argparse
import ntpath
import os
import re
import shutil
import subprocess
import sys

# "0x00007FF6A1B2C3D4, C:\...\clice.exe(0x00007FF6A1B20000) + 0x2C3D4 byte(s)",
# on every OS; " (interrupted)" after the frame a signal or an exception
# interrupted. LLVM's Windows trace, which older Windows logs hold, has the
# same lines without it.
MODULE_FRAME = re.compile(
    r"^\s*0x[0-9a-fA-F]+, (.+?)\(0x[0-9a-fA-F]+\) \+ 0x([0-9a-fA-F]+) byte\(s\)( \(interrupted\))?",
    re.MULTILINE,
)
# Older logs: "0  clice 0x00005ea84c157818", the un-symbolized dump LLVM
# writes when the crashing machine has no llvm-symbolizer, rebased by the
# "main executable base" line before it. Already-symbolized "#0 0x..." frames
# do not match and pass through untouched.
FRAME = re.compile(r"^\s*(\d+)\s+(\S+)\s+0x([0-9a-fA-F]+)")
BASE = re.compile(r"main executable base: 0x([0-9a-fA-F]+)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="crash log file (or - for stdin)")
    parser.add_argument(
        "--symbols",
        required=True,
        help="symbol file (clice.gsym / an unstripped clice / dSYM inner DWARF)",
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
        help="preferred image base of a Windows module (default: lld's for 64-bit EXEs)",
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

    if BASE.search(text) is None and MODULE_FRAME.search(text) is None:
        print(
            "error: no frame naming its module and no 'main executable base' line "
            "in the log; the crash predates base recording — addresses cannot be rebased",
            file=sys.stderr,
        )
        return 1

    # A log can hold several crash sections appended by respawned processes,
    # each with its own ASLR base — track the most recent one while scanning.
    base = None
    # Module frames carry no number; they are counted per crash section.
    module_frames = 0
    for line in text.splitlines():
        if "=== CRASH STACK TRACE ===" in line:
            module_frames = 0
        base_match = BASE.search(line)
        if base_match is not None:
            base = int(base_match.group(1), 16)
            module_frames = 0
            print(line)
            continue
        module_frame = MODULE_FRAME.match(line)
        if module_frame is not None:
            number = str(module_frames)
            module_frames += 1
            module = ntpath.basename(module_frame.group(1)).lower()
            stem, extension = os.path.splitext(module)
            if stem != args.module.lower():
                print(line)
                continue
            offset = int(module_frame.group(2), 16)
            if extension in (".exe", ".dll"):
                offset += args.image_base
            # A return address: its call is the byte before it.
            if module_frame.group(3) is None:
                offset -= 1
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
