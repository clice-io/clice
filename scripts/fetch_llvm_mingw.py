"""Fetch the llvm-mingw toolchain that provides the mingw-w64 sysroot.

    python scripts/fetch_llvm_mingw.py <dest>

Downloads the llvm-mingw release for this host into <dest> (skipped when
<dest>/bin already exists) and prints the root, which is what
CLICE_MINGW_ROOT expects. The release is pinned to the LLVM version of the
prebuilt package; the manifest check in cmake/llvm.cmake rejects a mismatch.
"""

import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

RELEASE = "20260908"  # LLVM 23.1.1


def asset_name() -> str:
    machine = platform.machine().lower()
    arch = "aarch64" if machine in ("arm64", "aarch64") else "x86_64"
    if sys.platform == "win32":
        return f"llvm-mingw-{RELEASE}-ucrt-{arch}.zip"
    if sys.platform == "darwin":
        return f"llvm-mingw-{RELEASE}-ucrt-macos-universal.tar.xz"
    return f"llvm-mingw-{RELEASE}-ucrt-ubuntu-22.04-{arch}.tar.xz"


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dest = Path(sys.argv[1]).resolve()
    if (dest / "bin").is_dir():
        print(dest)
        return 0

    name = asset_name()
    url = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{RELEASE}/{name}"
    print(f"Downloading {url}", file=sys.stderr)
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / name
        urllib.request.urlretrieve(url, archive)
        extracted = Path(tmp) / "extracted"
        if name.endswith(".zip"):
            with zipfile.ZipFile(archive) as z:
                z.extractall(extracted)
        else:
            # Python's tarfile has no lzma-multithreading; tar is faster and
            # keeps the symlinks the wrappers rely on.
            extracted.mkdir()
            subprocess.run(
                ["tar", "-xf", str(archive), "-C", str(extracted)], check=True
            )
        (root,) = [p for p in extracted.iterdir() if p.is_dir()]
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(root), str(dest))
    print(dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
