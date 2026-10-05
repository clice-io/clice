#!/usr/bin/env python3
"""Build clice with Bazel and lay it out in build/<type>/.

    python scripts/build.py <type> [//target ...] [bazel option ...]

<type> is a build type of .bazelrc: RelWithDebInfo or Debug. The targets
(//:dist by default: clice, unit_tests and the resource directory) land in
build/<type>/bin, the resource directory in build/<type>/lib/clang, the
layout clice finds its resource directory in: ../lib/clang next to its
own directory, the executable's real path. The tests, the packaging and
the editors run the programs from there.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESOURCE_DIR = "/lib/clang/"


def bazel(*args: str, capture: bool = False) -> str:
    # npm's bazelisk (package.json), with the Bazel of .bazelversion.
    npx = shutil.which("npx")
    if npx is None:
        sys.exit("error: npx not found; run `npm ci` in a pixi environment")
    result = subprocess.run(
        [npx, "--no", "bazel", *args],
        cwd=ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    if result.returncode != 0:
        sys.exit(result.returncode)
    return result.stdout or ""


def place(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    # Bazel replaces an output rather than rewriting it, so a link keeps
    # what was built; the output tree can be on another file system.
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def main(argv: list[str]) -> int:
    if not argv or argv[0].startswith("-"):
        print(__doc__.strip(), file=sys.stderr)
        return 64
    build_type, rest = argv[0], argv[1:]
    targets = [arg for arg in rest if arg.startswith("//")] or ["//:dist"]
    options = [f"--config={build_type}"] + [
        arg for arg in rest if not arg.startswith("//")
    ]

    bazel("build", *options, *targets)
    files = bazel("cquery", *options, "--output=files", *targets, capture=True).split()
    execroot = Path(bazel("info", *options, "execution_root", capture=True).strip())

    dest = ROOT / "build" / build_type
    for directory in ("bin", "lib"):
        shutil.rmtree(dest / directory, ignore_errors=True)
    for file in files:
        source = execroot / file
        _, resource, rest = file.partition(RESOURCE_DIR)
        if resource:
            place(source, dest / "lib" / "clang" / rest)
        else:
            place(source, dest / "bin" / source.name)
    print(f"build/{build_type}: {len(files)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
