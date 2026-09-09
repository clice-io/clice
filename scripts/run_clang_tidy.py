#!/usr/bin/env python3
"""Run clang-tidy in parallel on all files in compile_commands.json."""

import json
import os
import shlex
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


def split_command(command: str) -> list[str]:
    """Split a compile command the way the platform's shell would."""
    if os.name != "nt":
        return shlex.split(command)
    words = shlex.split(command, posix=False)
    return [w[1:-1] if len(w) >= 2 and w[0] == w[-1] == '"' else w for w in words]


def without_pch(arguments: list[str]) -> list[str]:
    """Drop the precompiled-header flags CMake adds for clang.

    The PCH only exists after a build, and clang-tidy re-parses the headers
    anyway; a `-Xclang X` pair is removed together.
    """
    out = []
    skip = False
    for i, arg in enumerate(arguments):
        if skip:
            skip = False
            continue
        if arg == "-Xclang" and i + 1 < len(arguments):
            follower = arguments[i + 1]
            if (
                follower in ("-include-pch", "-include", "-fno-pch-timestamp")
                or "cmake_pch" in follower
            ):
                skip = True
                continue
        if arg in ("-Winvalid-pch", "-fpch-instantiate-templates"):
            continue
        out.append(arg)
    return out


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build/RelWithDebInfo"
    cdb_path = Path(build_dir) / "compile_commands.json"

    if not cdb_path.exists():
        print(f"Error: {cdb_path} not found. Run cmake-config first.", file=sys.stderr)
        sys.exit(1)

    project_root = Path(__file__).resolve().parent.parent
    src_dirs = (project_root / "src", project_root / "tests")

    cdb = json.loads(cdb_path.read_text())
    for entry in cdb:
        arguments = entry.pop("arguments", None) or split_command(entry.pop("command"))
        entry["arguments"] = without_pch(arguments)
    tidy_dir = Path(build_dir) / "clang-tidy"
    tidy_dir.mkdir(exist_ok=True)
    (tidy_dir / "compile_commands.json").write_text(json.dumps(cdb))

    files = [
        entry["file"]
        for entry in cdb
        if any(Path(entry["file"]).resolve().is_relative_to(d) for d in src_dirs)
    ]
    total = len(files)

    lock = threading.Lock()
    done = 0
    failed = []

    def run(file: str) -> tuple[str, int, str]:
        result = subprocess.run(
            ["clang-tidy", "-p", str(tidy_dir), "--quiet", file],
            capture_output=True,
            text=True,
        )
        return file, result.returncode, result.stdout + result.stderr

    with ThreadPoolExecutor() as pool:
        futures = {pool.submit(run, f): f for f in files}
        for future in as_completed(futures):
            file, code, output = future.result()
            with lock:
                done += 1
                name = Path(file).name
                if code != 0:
                    failed.append(file)
                    print(f"[{done}/{total}] FAIL {name}")
                    if output.strip():
                        print(output, end="")
                else:
                    print(f"[{done}/{total}] OK   {name}")

    if failed:
        print(f"\nclang-tidy failed on {len(failed)}/{total} files.", file=sys.stderr)
        sys.exit(1)

    print(f"\nclang-tidy passed on {total} files.")


if __name__ == "__main__":
    main()
