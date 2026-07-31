#!/usr/bin/env python3
"""Assert a packaged clice binary only depends on system dynamic libraries.

macOS artifacts once silently linked conda's ``@rpath/libc++.1.dylib``; the
binary loaded fine on the build machine (where the conda env was present) but
would die anywhere else. Every test job passed because they all ran inside that
same env — only the editor smoke test, by accident, exercised the binary in a
fresh environment and failed with a 20-minute hang. This script turns that
accidental sentinel into an explicit gate: it inspects the shipped binary's
dynamic dependencies and rejects anything that resolves into a conda/pixi env
instead of the OS.

Both branches use LLVM binutils (``llvm-otool`` / ``llvm-readelf``), which ship
in the build env and can read Mach-O even from a Linux host.

TODO: read PE too. With the /MT switch (static CRT), the Windows artifacts
should no longer import ``MSVCP140.dll`` or ``VCRUNTIME140.dll``; add PE
support to verify this.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Linux NEEDED whitelist, derived from the real packaged x86_64-unknown-linux-gnu binary.
# clice statically links libstdc++/libgcc, so a portable build only pulls in the
# glibc runtime pieces. Notably libstdc++.so.6 / libc++.so are absent: if either
# ever appears it would resolve from the conda RUNPATH (see check_elf), which is
# exactly the shape of the macOS libc++ incident. Keeping them out of the
# whitelist makes that regression a hard failure.
LINUX_NEEDED_WHITELIST = {
    "libpthread.so.0",
    "librt.so.1",
    "libdl.so.2",
    "libm.so.6",
    "libc.so.6",
    "ld-linux-x86-64.so.2",
    "ld-linux-aarch64.so.1",
}

# Path fragments that indicate a dependency or search path pointing into a
# conda/pixi environment rather than the operating system.
CONDA_MARKERS = ("conda", ".pixi", "/envs/")

# macOS system library location prefixes. Anything outside these (notably
# @rpath / @loader_path / absolute conda paths) is a violation.
MACOS_SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")


def run_tool(args: list[str]) -> str:
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"{args[0]} failed (exit {result.returncode}):\n{result.stderr}"
        )
    return result.stdout


def assert_parsed(count: int, tool: str, what: str) -> list[str]:
    """Fail closed when a parse yielded nothing.

    Every real executable has at least one dynamic dependency, so parsing none
    means the tool's output format changed, not that the binary is clean.
    """
    if count:
        return []
    return [
        f"parsed no {what} from `{tool}` output; refusing to vouch for this "
        f"binary (the tool's output format may have changed)"
    ]


def detect_format(binary: Path) -> str:
    magic = binary.read_bytes()[:4]
    if magic[:4] == b"\x7fELF":
        return "elf"
    # Mach-O: 32/64-bit little/big endian, or a fat/universal archive.
    macho = {
        b"\xcf\xfa\xed\xfe",  # 64-bit LE
        b"\xce\xfa\xed\xfe",  # 32-bit LE
        b"\xfe\xed\xfa\xcf",  # 64-bit BE
        b"\xfe\xed\xfa\xce",  # 32-bit BE
        b"\xca\xfe\xba\xbe",  # fat/universal
    }
    if magic in macho:
        return "macho"
    raise RuntimeError(f"{binary}: unrecognized object file magic {magic!r}")


def check_elf(binary: Path) -> list[str]:
    """Return a list of violation messages for an ELF binary.

    The NEEDED whitelist is the real gate here: it rejects any C++ runtime
    (libstdc++/libc++) or other non-glibc dependency, which is what would let a
    conda-resolved library slip in.

    The conda RUNPATH that Linux conda-clang bakes in via its default config is
    reported for visibility but is not a hard failure: it is pre-existing,
    benign (it resolves nothing on a user's machine, and every NEEDED entry is
    system glibc), and removing it needs a separate toolchain change. macOS,
    where the config is stripped with --no-default-config, enforces the
    equivalent LC_RPATH rule strictly (see check_macho).
    """
    out = run_tool(["llvm-readelf", "-d", str(binary)])
    violations = []
    parsed = 0
    for line in out.splitlines():
        if "(NEEDED)" in line:
            m = re.search(r"Shared library: \[([^\]]+)\]", line)
            if not m:
                continue
            parsed += 1
            if m.group(1) not in LINUX_NEEDED_WHITELIST:
                violations.append(f"unexpected NEEDED dependency: {m.group(1)}")
        elif "(RUNPATH)" in line or "(RPATH)" in line:
            m = re.search(r"\[([^\]]+)\]", line)
            if m and any(marker in m.group(1) for marker in CONDA_MARKERS):
                print(f"note: RUNPATH references a conda/pixi env: {m.group(1)}")

    violations.extend(assert_parsed(parsed, "llvm-readelf -d", "NEEDED entries"))
    return violations


def check_macho(binary: Path) -> list[str]:
    """Return a list of violation messages for a Mach-O binary."""
    violations = []
    parsed = 0

    # Load dylib dependencies. The first line of -L output is the file-path
    # header ("<path>:"), not a dependency, so skip it.
    load_out = run_tool(["llvm-otool", "-L", str(binary)])
    lines = load_out.splitlines()
    for line in lines[1:]:
        line = line.strip()
        if not line or not line.endswith(")"):
            continue
        dylib = line.split(" (", 1)[0].strip()
        parsed += 1
        if not dylib.startswith(MACOS_SYSTEM_PREFIXES):
            violations.append(f"non-system dylib dependency: {dylib}")

    violations.extend(assert_parsed(parsed, "llvm-otool -L", "dylib dependencies"))

    # LC_RPATH entries must not point into a conda/pixi env.
    rpath_out = run_tool(["llvm-otool", "-l", str(binary)])
    for path in re.findall(r"cmd LC_RPATH.*?path (\S+)", rpath_out, re.DOTALL):
        if any(marker in path for marker in CONDA_MARKERS):
            violations.append(f"LC_RPATH points into a conda/pixi env: {path}")

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binary", type=Path, help="Path to the packaged clice binary to inspect."
    )
    args = parser.parse_args()

    binary: Path = args.binary
    if not binary.is_file():
        print(f"error: {binary} is not a file", file=sys.stderr)
        return 1

    fmt = detect_format(binary)
    violations = check_elf(binary) if fmt == "elf" else check_macho(binary)

    if violations:
        print(f"FAIL: {binary} has forbidden dynamic dependencies:", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        return 1

    print(f"OK: {binary} ({fmt}) depends only on system libraries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
