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


def check_package_cmake(path: Path) -> None:
    """Verify package.cmake has exactly one setup_llvm(...) call that the
    update script can rewrite. Used by CI to catch drift before the next bump."""
    text = path.read_text(encoding="utf-8")
    matches = re.findall(r'setup_llvm\("[^"]*"\)', text)
    if len(matches) == 0:
        print(f"Error: no setup_llvm(...) call found in {path}", file=sys.stderr)
        sys.exit(1)
    if len(matches) > 1:
        print(
            f"Error: expected exactly 1 setup_llvm(...) call in {path}, "
            f"found {len(matches)}: {matches}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"OK: {path} has a single setup_llvm(...) call: {matches[0]}")


def check_manifest(path: Path) -> None:
    """Verify the manifest is a well-formed non-empty array with required fields."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as err:
        print(f"Error: {path} is not valid JSON: {err}", file=sys.stderr)
        sys.exit(1)
    if not isinstance(data, list) or len(data) == 0:
        print(f"Error: {path} must be a non-empty JSON array", file=sys.stderr)
        sys.exit(1)
    required = ("version", "platform", "arch", "build_type", "filename", "sha256")
    for idx, entry in enumerate(data):
        missing = [k for k in required if k not in entry]
        if missing:
            print(
                f"Error: {path} entry {idx} is missing fields: {missing}",
                file=sys.stderr,
            )
            sys.exit(1)
    print(f"OK: {path} has {len(data)} well-formed entries")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Update LLVM version references in the clice project."
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Validate existing state without modifying files (for CI drift checks)",
    )
    parser.add_argument(
        "--version",
        help="New LLVM version string (e.g. 21.2.0); required unless --check",
    )
    parser.add_argument(
        "--manifest-src",
        help="Path to the source llvm-manifest.json; required unless --check",
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

    manifest_dest = Path(args.manifest_dest)
    package_cmake = Path(args.package_cmake)

    if not package_cmake.is_file():
        print(f"Error: package.cmake not found: {package_cmake}", file=sys.stderr)
        sys.exit(1)

    if args.check:
        check_package_cmake(package_cmake)
        check_manifest(manifest_dest)
        print("Done (check mode).")
        return

    if not args.version or not args.manifest_src:
        print(
            "Error: --version and --manifest-src are required unless --check is set",
            file=sys.stderr,
        )
        sys.exit(1)

    manifest_src = Path(args.manifest_src)
    if not manifest_src.is_file():
        print(f"Error: manifest source not found: {manifest_src}", file=sys.stderr)
        sys.exit(1)

    copy_manifest(manifest_src, manifest_dest)
    update_package_cmake(package_cmake, args.version)

    print("Done.")


if __name__ == "__main__":
    main()
