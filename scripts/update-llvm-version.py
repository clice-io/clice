#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path


def copy_manifest(src: Path, dest: Path) -> None:
    text = src.read_text(encoding="utf-8")

    try:
        data = json.loads(text)
    except json.JSONDecodeError as err:
        print(f"Error: {src} is not valid JSON: {err}", file=sys.stderr)
        sys.exit(1)

    if not isinstance(data, list) or len(data) == 0:
        print(f"Error: {src} must be a non-empty JSON array", file=sys.stderr)
        sys.exit(1)

    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")

    print(f"Copied manifest: {src} -> {dest} ({len(data)} entries)")


def update_package_cmake(path: Path, version: str) -> None:
    text = path.read_text(encoding="utf-8")

    pattern = r'setup_llvm\("[^"]*"\)'
    matches = re.findall(pattern, text)

    if len(matches) == 0:
        print(f"Error: no setup_llvm(...) call found in {path}", file=sys.stderr)
        sys.exit(1)

    if len(matches) > 1:
        print(
            f"Error: expected exactly 1 setup_llvm(...) call in {path}, "
            f"found {len(matches)}",
            file=sys.stderr,
        )
        sys.exit(1)

    old_call = matches[0]
    new_call = f'setup_llvm("{version}")'

    if old_call == new_call:
        print(f"Version in {path} is already {version}, no change needed")
        return

    updated = text.replace(old_call, new_call)
    path.write_text(updated, encoding="utf-8")
    print(f"Updated {path}: {old_call} -> {new_call}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Update LLVM version references in the clice project."
    )
    parser.add_argument(
        "--version",
        required=True,
        help="New LLVM version string (e.g. 21.2.0)",
    )
    parser.add_argument(
        "--manifest-src",
        required=True,
        help="Path to the source llvm-manifest.json (downloaded from build)",
    )
    parser.add_argument(
        "--manifest-dest",
        required=True,
        help="Path to destination manifest (e.g. config/llvm-manifest.json)",
    )
    parser.add_argument(
        "--package-cmake",
        required=True,
        help="Path to cmake/package.cmake",
    )
    args = parser.parse_args()

    manifest_src = Path(args.manifest_src)
    manifest_dest = Path(args.manifest_dest)
    package_cmake = Path(args.package_cmake)

    if not manifest_src.is_file():
        print(f"Error: manifest source not found: {manifest_src}", file=sys.stderr)
        sys.exit(1)

    if not package_cmake.is_file():
        print(f"Error: package.cmake not found: {package_cmake}", file=sys.stderr)
        sys.exit(1)

    copy_manifest(manifest_src, manifest_dest)
    update_package_cmake(package_cmake, args.version)

    print("Done.")


if __name__ == "__main__":
    main()
