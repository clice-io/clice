#!/usr/bin/env python3
"""
Validate the LLVM distribution component list against the actual LLVM source tree.

Scans the LLVM source for CMake library targets and compares them against
a components JSON file to detect stale or misspelled entries.
"""

import argparse
import difflib
import json
import re
import sys
from pathlib import Path


# CMake function calls that define library targets.
# The captured group uses [^\s)]+  to grab the target name without
# trailing parentheses or whitespace.
LLVM_LIB_PATTERNS = [
    re.compile(r"add_llvm_component_library\(\s*([^\s)]+)"),
    re.compile(r"add_llvm_library\(\s*([^\s)]+)"),
]

CLANG_LIB_PATTERNS = [
    re.compile(r"add_clang_library\(\s*([^\s)]+)"),
]

# Header-only / custom install targets.
HEADER_PATTERNS = [
    re.compile(r"add_llvm_install_targets\(\s*([^\s)]+)"),
    re.compile(r"add_custom_target\(\s*([^\s)]+)"),
]

# Targets we recognise as header-only distribution components.
KNOWN_HEADER_TARGETS = {
    "llvm-headers",
    "clang-headers",
    "clang-tidy-headers",
    "clang-resource-headers",
}


def scan_targets(directory: Path, patterns: list[re.Pattern]) -> set[str]:
    """Recursively scan *directory* for CMakeLists.txt files and extract target names."""
    targets: set[str] = set()
    if not directory.is_dir():
        return targets
    for cmake_file in directory.rglob("CMakeLists.txt"):
        text = cmake_file.read_text(errors="replace")
        for pattern in patterns:
            for match in pattern.finditer(text):
                targets.add(match.group(1))
    return targets


def scan_header_targets(llvm_src: Path) -> set[str]:
    """Scan for well-known header / custom-install targets across the tree."""
    found: set[str] = set()
    for cmake_file in llvm_src.rglob("CMakeLists.txt"):
        text = cmake_file.read_text(errors="replace")
        for pattern in HEADER_PATTERNS:
            for match in pattern.finditer(text):
                name = match.group(1)
                if name in KNOWN_HEADER_TARGETS:
                    found.add(name)
    return found


def collect_source_targets(llvm_src: Path) -> set[str]:
    """Return the full set of library / header targets found in the LLVM source tree."""
    targets: set[str] = set()
    targets |= scan_targets(llvm_src / "llvm" / "lib", LLVM_LIB_PATTERNS)
    targets |= scan_targets(llvm_src / "clang" / "lib", CLANG_LIB_PATTERNS)
    targets |= scan_targets(llvm_src / "clang-tools-extra", CLANG_LIB_PATTERNS)
    targets |= scan_header_targets(llvm_src)
    return targets


def load_components(path: Path) -> list[str]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if isinstance(data, dict):
        data = data.get("components", [])
    if not isinstance(data, list) or not data:
        print(f"Error: no component list found in {path}", file=sys.stderr)
        sys.exit(1)
    return data


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Validate LLVM distribution components against the source tree."
    )
    parser.add_argument(
        "--llvm-src",
        required=True,
        help="Path to the llvm-project source root",
    )
    parser.add_argument(
        "--components-file",
        required=True,
        help="Path to llvm-components.json",
    )
    args = parser.parse_args()

    llvm_src = Path(args.llvm_src).expanduser().resolve()
    components_file = Path(args.components_file).expanduser().resolve()

    if not llvm_src.is_dir():
        print(f"Error: LLVM source directory not found: {llvm_src}")
        sys.exit(1)

    if not (llvm_src / "llvm" / "CMakeLists.txt").exists():
        print(f"Error: {llvm_src} does not look like an llvm-project root.")
        sys.exit(1)

    if not components_file.is_file():
        print(f"Error: components file not found: {components_file}")
        sys.exit(1)

    components = load_components(components_file)
    source_targets = collect_source_targets(llvm_src)

    print(f"Found {len(source_targets)} targets in LLVM source tree")
    print(f"Components file lists {len(components)} entries")

    # --- Check for components that are missing from the source tree -----------
    missing: list[tuple[str, list[str]]] = []
    for name in components:
        if name not in source_targets:
            suggestions = difflib.get_close_matches(name, source_targets, n=3, cutoff=0.6)
            missing.append((name, suggestions))

    if missing:
        print(f"\nError: {len(missing)} component(s) not found in the source tree:\n")
        for name, suggestions in missing:
            print(f"  - {name}")
            if suggestions:
                print(f"    Did you mean: {', '.join(suggestions)}?")
        sys.exit(1)

    # --- Warn about source targets not present in the component list ----------
    component_set = set(components)
    new_targets = sorted(source_targets - component_set - KNOWN_HEADER_TARGETS)
    # Filter to targets that follow LLVM/Clang naming conventions to reduce noise.
    noteworthy = [
        t for t in new_targets if t.startswith(("LLVM", "clang", "Clang"))
    ]
    if noteworthy:
        print(f"\nWarning: {len(noteworthy)} target(s) in source not listed in components:")
        for name in noteworthy:
            print(f"  + {name}")

    print("\nAll components validated successfully.")
    sys.exit(0)


if __name__ == "__main__":
    main()
