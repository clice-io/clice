/// Holds every timer of src/ and unittests/ to the waiting rule of
/// tests/README.md: a wait on the clock for something that has an event
/// passes or fails by the machine's load. A timer (async::sleep,
/// async::with_timeout, the start of an async::timer,
/// this_thread::sleep_for or sleep_until, a request's timeout, a test
/// loop's watchdog) is allowed where the comment block right above the
/// statement it is in starts a line with a label, the same ones the
/// TypeScript lint rule clice/timers (tools/lint/rules.ts) takes:
///
///     // Hang detection: <why>      a bound only a hang reaches
///     // Time is the feature: <why> a debounce, a grace period, a pace
///     // No event: <why>            what nothing announces: a condition
///                                   re-checked until it holds, or an end
///
/// It needs no build.
///
/// Usage:
///   node tools/precheck/timers.ts

import * as fs from "node:fs";
import * as path from "node:path";
import { LABELS } from "../lint/rules.ts";
import { REPO_ROOT } from "../repo.ts";

const SOURCES = new Set([".cpp", ".cppm", ".h", ".cc"]);

const TIMER =
    /\basync::sleep\(|\basync::with_timeout\(|timer->start\(|\bsleep_for\(|\bsleep_until\(|\btimeout = chrono::|\bwatchdog = chrono::/;
const LABEL = new RegExp(String.raw`^\s*//+\s*(?:${LABELS.join("|")}) \S`);
const COMMENT = /^\s*\/\//;
/// A zero wait is a turn of the event loop; test_jitter() is the test
/// hook's own delay, zero without it.
const EXEMPT = /\((?:chrono::)?milliseconds\(0\)\)|\(test_jitter\(\)\)/;
const STATEMENT_BOUNDARY = /[;{}]\s*$|^\s*\/\//;

function labelled(lines: readonly string[], index: number): boolean {
    // Back to the first line of the statement: the one after a line that
    // ends one, opens a block or is a comment.
    let first = index;
    while (first > 0 && !STATEMENT_BOUNDARY.test(lines[first - 1] ?? "")) {
        first -= 1;
    }
    for (let above = first - 1; above >= 0 && COMMENT.test(lines[above] ?? ""); above -= 1) {
        if (LABEL.test(lines[above] ?? "")) {
            return true;
        }
    }
    return false;
}

/// The timers of a source that no label allows, each with its line.
export function unlabelledTimers(file: string, text: string): string[] {
    const lines = text.split(/\r\n?|\n/);
    return lines.flatMap((line, index) =>
        COMMENT.test(line) || !TIMER.test(line) || EXEMPT.test(line) || labelled(lines, index)
            ? []
            : [`${file}:${index + 1}: ${line.trim()}`],
    );
}

/// The files under a directory, sorted.
function filesUnder(directory: string): string[] {
    return fs
        .readdirSync(directory, { recursive: true, encoding: "utf8" })
        .sort()
        .map((file) => path.join(directory, file));
}

function main(): void {
    const failures = ["src", "unittests"].flatMap((directory) =>
        filesUnder(path.join(REPO_ROOT, directory))
            .filter((file) => SOURCES.has(path.extname(file)))
            .flatMap((file) =>
                unlabelledTimers(path.relative(REPO_ROOT, file), fs.readFileSync(file, "utf8")),
            ),
    );
    for (const failure of failures) {
        console.log(failure);
    }
    if (failures.length > 0) {
        console.error(
            "\nA timer waiting for something that has an event fails on a loaded machine: " +
                "wait on the event. Any other timer follows a comment block with a line " +
                "`// Hang detection: <why>`, `// Time is the feature: <why>` or " +
                "`// No event: <why>` (see tools/precheck/timers.ts).",
        );
        process.exitCode = 1;
    }
}

if (import.meta.main) {
    main();
}
