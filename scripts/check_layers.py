#!/usr/bin/env python3
"""Enforce the src/ include layering: core <- config <- {project, worker} <- sched <- server,
project <- analysis and {analysis, server} <- driver; inside core, support <- vfs <-
command <- syntax <- semantic <- compile <- index <- feature.

Each layer may include, or import the partitions of, downward only: clice is one
module, clice, whose partitions are named by their path under src/
(`import :sched.graph;`), which the check verifies too. Bazel enforces the layering
between its targets, as it checks that every header a source includes belongs to the
target or its dependencies; the core directories share one target, so only this check
keeps them acyclic, as partition imports must be. It needs no build.
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
        + [
            "config/",
            "project/",
            "worker/",
            "sched/",
            "server/",
            "analysis/",
            "driver/",
        ]
        for index, layer in enumerate(CORE)
    },
    "config": ["project/", "worker/", "sched/", "server/", "analysis/", "driver/"],
    "project": ["worker/", "sched/", "server/", "analysis/", "driver/"],
    "worker": ["project/", "sched/", "server/", "analysis/", "driver/"],
    "sched": ["server/", "analysis/", "driver/"],
    "server": ["analysis/", "driver/"],
    "analysis": ["worker/", "sched/", "server/", "driver/"],
}

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')
IMPORT = re.compile(r"^\s*(?:export\s+)?import\s+:([\w.]+)\s*;")
PARTITION = re.compile(r"^\s*(?:export\s+)?module\s+clice:([\w.]+)\s*;")


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
    for path in sorted(src.rglob("*.cppm")):
        name = ".".join(path.relative_to(src).with_suffix("").parts)
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            declared = PARTITION.match(line)
            if declared and declared.group(1) != name:
                violations.append(
                    f"{path.relative_to(src.parent)}:{number}: "
                    f"the partition of this file is named {name}"
                )
    for violation in violations:
        print(violation)
    if violations:
        print(
            f"\n{len(violations)} layering violation(s): "
            "core <- config <- {project, worker} <- sched <- server, project <- analysis, "
            "{analysis, server} <- driver; inside core, "
            + " <- ".join(CORE)
            + "; includes and imports go downward only."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
