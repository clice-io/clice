"""Prepare test data fixtures for editor E2E tests.

Usage: python tests/prepare.py <fixture> [<fixture> ...]

Each fixture is a subdirectory of tests/data. Fixtures with a
CMakeLists.txt get compile_commands.json generated via CMake; plain
fixtures (e.g. hello_world) are covered by generate_test_data_cdbs.
Stale .clice caches are removed so every run starts fresh.
"""

import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from tests.cdb import generate_cdb, generate_test_data_cdbs  # noqa: E402


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
        clice_dir = path / ".clice"
        if clice_dir.exists():
            shutil.rmtree(clice_dir)
        print(f"prepared {fixture}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
