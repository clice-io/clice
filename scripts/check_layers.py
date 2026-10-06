#!/usr/bin/env python3
"""Enforce the src/ include layering: core <- config <- {project, worker} <- sched <- server,
and project <- analysis; inside core, support <- vfs <- command <- syntax <- semantic <-
compile <- index <- feature.

Each layer may include, or import the partitions of, downward only: clice is one module,
clice, whose partitions are named by their path under src/ (`import :sched.graph;`). Bazel enforces the layering between its targets too,
as it checks that every header a source includes belongs to the target or its
dependencies; the core directories share one target, so only this check keeps them
acyclic, which each needs to become a module. It needs no build.
"""

import re
import sys
from pathlib import Path

CORE = [
    "support",
    "vfs",
    "command",
    "syntax",
    "semantic",
    "compile",
    "index",
    "feature",
]

# Directory -> prefixes its sources must never include.
FORBIDDEN = {
    **{
        layer: [f"{above}/" for above in CORE[index + 1 :]]
        + ["config/", "project/", "worker/", "sched/", "server/", "analysis/"]
        for index, layer in enumerate(CORE)
    },
    "config": ["project/", "worker/", "sched/", "server/", "analysis/"],
    "project": ["worker/", "sched/", "server/", "analysis/"],
    "worker": ["project/", "sched/", "server/", "analysis/"],
    "sched": ["server/", "analysis/"],
    "server": ["analysis/"],
    "analysis": ["worker/", "sched/", "server/"],
}

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')
IMPORT = re.compile(r"^\s*(?:export\s+)?import\s+:([\w.]+)\s*;")


def main() -> int:
    src = Path(__file__).resolve().parent.parent / "src"
    violations = []
    for layer, banned in FORBIDDEN.items():
        for path in sorted((src / layer).rglob("*")):
            if path.suffix not in (".h", ".cpp", ".cc", ".cppm"):
                continue
            for number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), 1
            ):
                match = INCLUDE.match(line)
                imported = IMPORT.match(line)
                if not match and not imported:
                    continue
                header = (
                    match.group(1) if match else imported.group(1).replace(".", "/")
                )
                for prefix in banned:
                    if header.startswith(prefix):
                        violations.append(
                            f"{path.relative_to(src.parent)}:{number}: "
                            f"{layer}/ must not include {header}"
                        )
    for violation in violations:
        print(violation)
    if violations:
        print(
            f"\n{len(violations)} layering violation(s): "
            "core <- config <- {project, worker} <- sched <- server, project <- analysis; "
            "inside core, " + " <- ".join(CORE) + "; includes go downward only."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
