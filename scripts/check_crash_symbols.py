#!/usr/bin/env python3
"""Check that a crash of a release package symbolizes against its symbols.

What a user's crash report goes through: the package's clice, stripped,
crashes (`#pragma clang __debug crash`, which only CLICE_TEST_PRAGMA_CRASH
lets through), and scripts/symbolize.py resolves its trace with the symbol
package's GSYM. The frame the crash interrupted must name the clang function
that crashed, and the frames under it a function of clice's and main.

Usage:
    python scripts/check_crash_symbols.py <directory>

The directory holds what a packaged CI leg uploads: clice.tar.gz or
clice.zip, and clice-symbol.tar.xz or clice-symbol.zip.
"""

import argparse
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

# A symbolized frame, "#3 0x1401b94a0 0x00000001401b94a0: Lex + 32 @ ...",
# and the functions inlined into it on the lines under it,
# "    LexTokenInternal + 35685 @ ... [inlined]". GSYM keeps names mangled,
# but on Windows those clang declares in an anonymous namespace, which have
# their bare name.
SYMBOLIZED_FRAME = re.compile(r"^#(\d+) 0x[0-9a-f]+ 0x[0-9a-fA-F]+: (\S+) \+ \d+ @")
INLINED = re.compile(r"^\s+(\S+) \+ \d+ @")


def unpack(directory: Path, stem: str, into: Path) -> None:
    for name in (f"{stem}.tar.gz", f"{stem}.tar.xz"):
        if (directory / name).is_file():
            with tarfile.open(directory / name) as archive:
                archive.extractall(into, filter="data")
            return
    if (directory / f"{stem}.zip").is_file():
        with zipfile.ZipFile(directory / f"{stem}.zip") as archive:
            archive.extractall(into)
        return
    sys.exit(f"error: no {stem} archive in {directory}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        unpack(args.directory, "clice", root / "package")
        unpack(args.directory, "clice-symbol", root / "symbols")
        executable = (
            root
            / "package"
            / "clice"
            / "bin"
            / ("clice.exe" if os.name == "nt" else "clice")
        )
        executable.chmod(0o755)
        symbols = next((root / "symbols").rglob("*.gsym"), None)
        if symbols is None:
            sys.exit("error: no .gsym in the symbol package")

        (root / "main.cpp").write_text(
            "#pragma clang __debug crash\nint main() { return 0; }\n"
        )
        crashed = subprocess.run(
            [str(executable), "inspect", "document-symbols", "main.cpp"],
            cwd=root,
            env={**os.environ, "CLICE_TEST_PRAGMA_CRASH": "1"},
            capture_output=True,
            text=True,
            timeout=300,
        )
        log = root / "crash.log"
        log.write_text(crashed.stderr)
        if "=== CRASH STACK TRACE ===" not in crashed.stderr:
            print(crashed.stderr)
            sys.exit(
                f"error: the package's clice ended with {crashed.returncode} and no crash trace"
            )

        symbolized = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("symbolize.py")),
                str(log),
                "--symbols",
                str(symbols),
            ],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        print(symbolized)

    # The functions of each frame, the inlined ones too.
    frames: dict[int, list[str]] = {}
    number = None
    for line in symbolized.splitlines():
        if match := SYMBOLIZED_FRAME.match(line):
            number = int(match.group(1))
            frames[number] = [match.group(2)]
        elif number is not None and (match := INLINED.match(line)):
            frames[number].append(match.group(1))
        else:
            number = None

    under = [name for frame, names in frames.items() if frame > 0 for name in names]
    failures = []
    if not any(
        name == "HandlePragma" or "18PragmaDebugHandler12HandlePragma" in name
        for name in frames.get(0, [])
    ):
        failures.append("the interrupted frame is not clang's HandlePragma")
    if not any(name.startswith("_ZN5clice") for name in under):
        failures.append("no frame under it is clice's")
    if "main" not in under:
        failures.append("no frame under it is main")
    for failure in failures:
        print(f"error: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
