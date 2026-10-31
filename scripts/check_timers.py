#!/usr/bin/env python3
"""Hold every timer of src/ and unittests/ to the waiting rule of tests/README.md:
a wait on the clock for something that has an event passes or fails by the
machine's load. A timer (async::sleep, async::with_timeout, the start of an
async::timer, std::this_thread::sleep_for or sleep_until, a request's timeout,
a test loop's watchdog) is allowed where the comment block right above the
statement it is in starts a line with a label, the same ones the TypeScript
lint rule clice/timers (tools/lint/rules.ts) takes:

    // Hang detection: <why>      a bound only a hang reaches
    // Time is the feature: <why> a debounce, a grace period, a pace
    // No event: <why>            what nothing announces: a condition
                                  re-checked until it holds, or an end
It needs no build.
"""

import re
import sys
from pathlib import Path

TIMER = re.compile(
    r"\basync::sleep\(|\basync::with_timeout\(|timer->start\(|\bsleep_for\(|\bsleep_until\("
    r"|\btimeout = std::chrono::|\bwatchdog = std::chrono::"
)
LABEL = re.compile(r"^\s*//+\s*(Hang detection|Time is the feature|No event): \S")
COMMENT = re.compile(r"^\s*//")
# A zero wait is a turn of the event loop; test_jitter() is the test hook's
# own delay, zero without it.
EXEMPT = re.compile(r"\((?:std::chrono::)?milliseconds\(0\)\)|\(test_jitter\(\)\)")


def labelled(lines: list[str], index: int) -> bool:
    # Back to the first line of the statement: the one after a line that
    # ends one, opens a block or is a comment.
    while index > 0 and not (
        lines[index - 1].rstrip().endswith((";", "{", "}"))
        or COMMENT.match(lines[index - 1])
    ):
        index -= 1
    above = index - 1
    while above >= 0 and COMMENT.match(lines[above]):
        if LABEL.match(lines[above]):
            return True
        above -= 1
    return False


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    failures = []
    for directory in ("src", "unittests"):
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".cpp", ".cppm", ".h", ".cc"):
                continue
            lines = path.read_text(encoding="utf-8").splitlines()
            for index, line in enumerate(lines):
                if COMMENT.match(line) or not TIMER.search(line) or EXEMPT.search(line):
                    continue
                if not labelled(lines, index):
                    failures.append(
                        f"{path.relative_to(root)}:{index + 1}: {line.strip()}"
                    )
    for failure in failures:
        print(failure)
    if failures:
        print(
            "\nA timer waiting for something that has an event fails on a loaded machine: "
            "wait on the event. Any other timer follows a comment block with a line "
            "`// Hang detection: <why>`, `// Time is the feature: <why>` or "
            "`// No event: <why>` (see scripts/check_timers.py).",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
