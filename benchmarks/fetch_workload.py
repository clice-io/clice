#!/usr/bin/env python3
"""Materialize a pinned benchmark workload from benchmarks/workloads.json.

Usage:
    python benchmarks/fetch_workload.py <name>

Clones the workload's repository at its pinned ref into
benchmarks/workloads/<name> (shallow) and runs the configure command to
generate compile_commands.json. Both steps are skipped when already done,
so re-running is cheap.
"""

import json
import subprocess
import sys
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent


def run(args: list[str], cwd: Path) -> None:
    print(f"+ {' '.join(args)}")
    subprocess.run(args, cwd=cwd, check=True)


def main() -> int:
    workloads = json.loads((BENCH_DIR / "workloads.json").read_text())["workloads"]

    if len(sys.argv) != 2 or sys.argv[1] not in workloads:
        names = ", ".join(sorted(workloads))
        print(f"usage: fetch_workload.py <name>  (available: {names})")
        return 1

    name = sys.argv[1]
    workload = workloads[name]
    root = BENCH_DIR / "workloads" / name

    if not (root / ".git").exists():
        root.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "--quiet"], cwd=root)
        run(["git", "remote", "add", "origin", workload["git"]], cwd=root)
        run(
            ["git", "fetch", "--depth", "1", "origin", workload["ref"]],
            cwd=root,
        )
        run(["git", "checkout", "--quiet", "FETCH_HEAD"], cwd=root)
    else:
        print(f"{root} already cloned")

    cdb = root / workload["cdb"]
    if not cdb.exists():
        run(workload["configure"], cwd=root)
    else:
        print(f"{cdb} already generated")

    workspace = cdb.parent
    print(f"\nworkload ready: {root}")
    print(f"compile_commands.json: {cdb}")
    print("suggested runs:")
    print(f"  ./build/RelWithDebInfo/bin/scan_benchmark {cdb}")
    print(f"  ./build/RelWithDebInfo/bin/pipeline_benchmark --limit 100 {cdb}")
    scenario = workload.get("scenario")
    if scenario:
        print(
            f"  node tools/bench/bench.ts --workspace {workspace}"
            f" --file {root / scenario['file']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
