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


def head_commit(root: Path) -> str | None:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=root,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def tracked_files_dirty(root: Path) -> bool:
    """Untracked files are invisible here on purpose: the configure step
    always leaves build output (including the CDB) untracked in the
    checkout, so only tracked-file modifications are detectable drift."""
    result = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=no"],
        cwd=root,
        capture_output=True,
        text=True,
    )
    return result.returncode != 0 or result.stdout.strip() != ""


def checkout(root: Path, workload: dict) -> bool:
    """Materialize the pinned ref; returns True when the tree changed.

    The marker file records which ref+commit a completed checkout
    produced: a fetch that died halfway, a ref bumped in workloads.json,
    or a checkout moved by hand all fail the comparison and are redone.
    A matching marker with locally modified tracked files is hard-reset —
    edits must not leak into a run the script reports as pinned.
    """
    marker = root / ".git" / "workload-ref"
    if (root / ".git").exists():
        head = head_commit(root)
        if head is not None and marker.exists():
            if marker.read_text() == f"{workload['ref']} {head}":
                if not tracked_files_dirty(root):
                    print(f"{root} already at {workload['ref']}")
                    return False
                print(f"{root} has local modifications; resetting")
                run(["git", "reset", "--hard", "--quiet", "HEAD"], cwd=root)
                return True

    root.mkdir(parents=True, exist_ok=True)
    run(["git", "init", "--quiet"], cwd=root)
    run(
        ["git", "fetch", "--depth", "1", workload["git"], workload["ref"]],
        cwd=root,
    )
    run(["git", "checkout", "--quiet", "FETCH_HEAD"], cwd=root)
    marker.write_text(f"{workload['ref']} {head_commit(root)}")
    return True


def main() -> int:
    workloads = json.loads((BENCH_DIR / "workloads.json").read_text())["workloads"]

    if len(sys.argv) != 2 or sys.argv[1] not in workloads:
        names = ", ".join(sorted(workloads))
        print(f"usage: fetch_workload.py <name>  (available: {names})")
        return 1

    name = sys.argv[1]
    workload = workloads[name]
    root = BENCH_DIR / "workloads" / name

    fresh = checkout(root, workload)

    cdb = root / workload["cdb"]
    if fresh or not cdb.exists():
        run(workload["configure"], cwd=root)
    else:
        print(f"{cdb} already generated")

    print(f"\nworkload ready: {root}")
    print(f"compile_commands.json: {cdb}")
    print("suggested runs:")
    print(f"  ./build/RelWithDebInfo/bin/scan_benchmark {cdb}")
    print(f"  ./build/RelWithDebInfo/bin/pipeline_benchmark --limit 100 {cdb}")
    scenario = workload.get("scenario")
    if scenario:
        print(
            f"  node tools/bench/bench.ts --workspace {root}"
            f" --file {root / scenario['file']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
