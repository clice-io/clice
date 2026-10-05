#!/usr/bin/env python3
"""Enforce the src/ include layering: core <- config <- {project, worker} <- sched <- server,
and project <- analysis.

Each layer may include downward only. Bazel enforces it too, as it checks that every
header a source includes belongs to the target or its dependencies; this check needs no
build.
"""

import re
import sys
from pathlib import Path

CORE = ["support", "syntax", "command", "compile", "semantic", "index", "feature"]

# Directory -> prefixes its sources must never include.
FORBIDDEN = {
    **{
        layer: ["config/", "project/", "worker/", "sched/", "server/", "analysis/"]
        for layer in CORE
    },
    "config": ["project/", "worker/", "sched/", "server/", "analysis/"],
    "project": ["worker/", "sched/", "server/", "analysis/"],
    "worker": ["project/", "sched/", "server/", "analysis/"],
    "sched": ["server/", "analysis/"],
    "server": ["analysis/"],
    "analysis": ["worker/", "sched/", "server/"],
}

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')


def main() -> int:
    src = Path(__file__).resolve().parent.parent / "src"
    violations = []
    for layer, banned in FORBIDDEN.items():
        for path in sorted((src / layer).rglob("*")):
            if path.suffix not in (".h", ".cpp", ".cc"):
                continue
            for number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), 1
            ):
                match = INCLUDE.match(line)
                if not match:
                    continue
                header = match.group(1)
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
            "core <- config <- {project, worker} <- sched <- server, project <- analysis, "
            "includes go downward only."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
