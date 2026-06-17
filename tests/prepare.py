"""Prepare test data fixtures for editor E2E tests.

Usage: python tests/prepare.py <fixture> [<fixture> ...]

Each fixture is a subdirectory of tests/data. Fixtures with a
CMakeLists.txt get compile_commands.json generated via CMake; plain
fixtures (e.g. hello_world) are covered by generate_test_data_cdbs.
Stale .clice caches are removed so every run starts fresh.
"""

import os
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tests.cdb import generate_cdb, generate_test_data_cdbs  # noqa: E402


def xdg_cache_dir(workspace: Path) -> Path | None:
    """Return the XDG cache directory clice would use for *workspace*.

    Mirrors resolvexdg_cache_dir() in src/server/workspace/config.cpp:
    $XDG_CACHE_HOME/clice/<xxh3_64 hash>  or  ~/.cache/clice/<hash>.
    We approximate xxh3_64 with the same 16-hex-digit format.
    """
    base = os.environ.get("XDG_CACHE_HOME") or ""
    if not base:
        home = os.environ.get("HOME") or ""
        if not home:
            return None
        base = os.path.join(home, ".cache")
    # xxh3_64bits is not available in stdlib; use xxhash if present,
    # otherwise fall back to a brute-force glob.
    try:
        import xxhash

        h = xxhash.xxh3_64(str(workspace).encode()).intdigest()
        return Path(base) / "clice" / f"{h:016x}"
    except ImportError:
        return Path(base) / "clice"


def clean_cache(workspace: Path) -> None:
    """Remove clice caches for *workspace* (both in-tree and XDG)."""
    clice_dir = workspace / ".clice"
    if clice_dir.exists():
        shutil.rmtree(clice_dir)

    xdg = xdg_cache_dir(workspace)
    if xdg is None:
        return
    if xdg.name == "clice":
        # No xxhash — glob all hash subdirectories.
        if xdg.is_dir():
            for child in xdg.iterdir():
                if child.is_dir() and len(child.name) == 16:
                    shutil.rmtree(child)
    elif xdg.is_dir():
        shutil.rmtree(xdg)


def main(fixtures: list[str]) -> int:
    if not fixtures:
        print(__doc__, file=sys.stderr)
        return 64

    data_dir = REPO_ROOT / "tests" / "data"
    generate_test_data_cdbs(data_dir)

    for fixture in fixtures:
        path = data_dir / fixture
        if not path.is_dir():
            print(f"error: no such fixture: {path}", file=sys.stderr)
            return 1
        if (path / "CMakeLists.txt").exists():
            generate_cdb(path)
        cdbs = [
            path / "compile_commands.json",
            path / "build" / "compile_commands.json",
        ]
        if not any(cdb.exists() for cdb in cdbs):
            print(f"error: no compile_commands.json for {fixture}", file=sys.stderr)
            return 1
        clean_cache(path)
        print(f"prepared {fixture}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
