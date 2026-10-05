#!/usr/bin/env python3
"""Write compile_commands.json for clice's own sources, from Bazel's actions.

    python scripts/compile_commands.py [<type>] [bazel option ...]

<type> is a build type of .bazelrc, RelWithDebInfo by default. The database
goes to the workspace root, where clice finds it. Its commands are Bazel's
compile commands as they run in the execution root, with the workspace
root as their directory: the paths into other repositories and Bazel's
outputs resolve there through the `bazel-out` link Bazel keeps in the
workspace and the `external` link this script adds, and the sources are
the workspace's own files.
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGETS = "//:dist + //:benchmarks"


def bazel(*args: str) -> str:
    npx = shutil.which("npx")
    if npx is None:
        sys.exit("error: npx not found; run `npm ci` in a pixi environment")
    result = subprocess.run(
        [npx, "--no", "bazel", *args],
        cwd=ROOT,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
    )
    if result.returncode != 0:
        sys.exit(result.returncode)
    return result.stdout


def link_external(execroot: Path) -> None:
    link = ROOT / "external"
    target = execroot / "external"
    if link.is_symlink() or link.is_junction():
        link.unlink()
    try:
        link.symlink_to(target, target_is_directory=True)
    except OSError:
        # Windows without the symlink privilege: a junction needs none.
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            check=True,
            capture_output=True,
        )


def main(argv: list[str]) -> int:
    build_type = argv[0] if argv and not argv[0].startswith("-") else "RelWithDebInfo"
    options = [f"--config={build_type}"] + [arg for arg in argv if arg.startswith("-")]

    query = json.loads(
        bazel(
            "aquery",
            *options,
            f'mnemonic("CppCompile", deps({TARGETS}))',
            "--output=jsonproto",
        )
    )
    execroot = Path(bazel("info", *options, "execution_root").strip())

    entries = []
    for action in query.get("actions", []):
        arguments = action["arguments"]
        source = arguments[arguments.index("-c") + 1]
        # clice's own sources; the generated version.cpp and other
        # repositories' files are no one's to edit here.
        if source.startswith(("external/", "bazel-out/")):
            continue
        entries.append(
            {"directory": ROOT.as_posix(), "file": source, "arguments": arguments}
        )
    entries.sort(key=lambda entry: entry["file"])

    link_external(execroot)
    (ROOT / "compile_commands.json").write_text(
        json.dumps(entries, indent=2) + "\n", encoding="utf-8"
    )
    print(f"compile_commands.json: {len(entries)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
