#!/usr/bin/env python3
"""Package build/<type> for release: the archive and the symbol package.

    python scripts/package.py <type>

Run after `pixi run build <type>`: it packages the very binary the test
suites ran, there is no separate release build. Stripping works on a copy,
so build/<type>/bin/clice keeps its debug info. It writes, in build/<type>:

    clice.tar.gz | clice.zip       clice/{bin/clice, lib/clang, clice.toml, LICENSE}
    clice-symbol.tar.xz | .zip     clice.gsym, for scripts/symbolize.py
    pack-symbol/clice.debug        the full DWARF (clice.dSYM on macOS)

The tools are the LLVM tools of pixi's xclang (dsymutil and strip on macOS).
The symbol package carries GSYM, not DWARF: it keeps everything crash
symbolization needs (functions, lines, inline chains) at ~1/10 the size.
"""

import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

from build import ROOT, remove


def archive(output: Path, directory: Path) -> None:
    """The files of directory, under their paths relative to it."""
    files = sorted(path for path in directory.rglob("*") if path.is_file())
    if output.name.endswith(".zip"):
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as zip_file:
            for path in files:
                zip_file.write(path, path.relative_to(directory).as_posix())
    elif output.name.endswith(".tar.gz"):
        with tarfile.open(output, "w:gz") as tar:
            for path in files:
                tar.add(path, path.relative_to(directory).as_posix())
    else:
        # Python's xz is single-threaded; xz -T0 is much faster on large
        # inputs, and every non-Windows host has it (pixi's xz).
        with open(output, "wb") as out:
            tar = subprocess.Popen(
                ["tar", "cf", "-", "."], cwd=directory, stdout=subprocess.PIPE
            )
            subprocess.run(
                ["xz", "-T0", "-c"], stdin=tar.stdout, stdout=out, check=True
            )
            if tar.wait() != 0:
                sys.exit(f"error: tar of {directory} failed")


def gsym(dwarf: Path, output: Path, log: Path) -> None:
    # --merged-functions: ICF folds identical functions onto one address
    # range; without it only one of the folded names survives conversion.
    # The same folding trips one line-table warning per folded DIE, which
    # `--quiet` does not cover ("duplicate line table detected", a DIE dump
    # each): on the macOS dSYM they add up to gigabytes, so they go to a log
    # file, not the build log.
    with open(log, "w") as out:
        result = subprocess.run(
            [
                "llvm-gsymutil",
                "--convert",
                dwarf,
                "--merged-functions",
                "--quiet",
                "--out-file",
                output,
            ],
            stdout=out,
            stderr=subprocess.STDOUT,
        )
    if result.returncode != 0:
        tail = log.read_bytes()[-4000:].decode(errors="replace")
        sys.exit(
            f"error: llvm-gsymutil failed ({result.returncode}); end of {log}:\n{tail}"
        )


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip(), file=sys.stderr)
        return 64
    build = ROOT / "build" / argv[0]
    windows = sys.platform == "win32"
    macos = sys.platform == "darwin"
    binary = build / "bin" / ("clice.exe" if windows else "clice")
    symbols = build / "pack-symbol"
    stripped = symbols / "stripped" / binary.name

    remove(symbols)
    stripped.parent.mkdir(parents=True)
    # Copies, not copy2: Bazel's outputs are read-only, the release is not.
    shutil.copyfile(binary, stripped)
    stripped.chmod(0o755)
    if macos:
        # The DWARF stays in the object files; dsymutil collects it from
        # them, by the paths the link recorded (relative to the execution
        # root, bazel-out of the workspace).
        dsym = symbols / "clice.dSYM"
        subprocess.run(["dsymutil", binary, "-o", dsym], cwd=ROOT, check=True)
        subprocess.run(["strip", "-x", stripped], check=True)
        dwarf = dsym / "Contents" / "Resources" / "DWARF" / "clice"
    else:
        # DWARF on Windows as well: MinGW binaries carry it like ELF ones.
        dwarf = symbols / "clice.debug"
        subprocess.run(["llvm-objcopy", "--only-keep-debug", binary, dwarf], check=True)
        subprocess.run(
            ["llvm-strip", "--strip-debug", "--strip-unneeded", stripped], check=True
        )
        subprocess.run(
            ["llvm-objcopy", f"--add-gnu-debuglink={dwarf}", stripped], check=True
        )

    pack = build / "pack"
    remove(pack)
    (pack / "clice" / "bin").mkdir(parents=True)
    shutil.copy2(stripped, pack / "clice" / "bin")
    shutil.copytree(
        build / "lib" / "clang",
        pack / "clice" / "lib" / "clang",
        copy_function=shutil.copyfile,
    )
    shutil.copyfile(ROOT / "docs" / "clice.toml", pack / "clice" / "clice.toml")
    shutil.copyfile(ROOT / "LICENSE", pack / "clice" / "LICENSE")
    archive(build / ("clice.zip" if windows else "clice.tar.gz"), pack)

    gsym_dir = symbols / "pack"
    gsym_dir.mkdir()
    gsym(dwarf, gsym_dir / "clice.gsym", symbols / "gsymutil.log")
    archive(
        build / ("clice-symbol.zip" if windows else "clice-symbol.tar.xz"), gsym_dir
    )
    print(f"packaged {binary}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
