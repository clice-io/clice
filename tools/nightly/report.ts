/// A job's results as a person reads them: what failed, each with the
/// command that reproduces it, and the numbers beside the previous night's.

import type { Failure, Results } from "./nightly.ts";

const KINDS: Record<Failure["kind"], string> = {
    setup: "The project could not be prepared",
    crash: "Crashes",
    hang: "Hangs",
    unfinished: "Runs of clice index that did not finish",
    index: "Units that failed to index, which the compiler compiles",
    compile: "Units clice reports errors on, which the compiler compiles",
    disagree: "Places where inspect and the server answer differently",
    inspect: "Requests clice inspect failed",
    server: "Requests the server failed",
    anomaly: "Anomalies",
    runner: "Errors of the nightly runner itself",
};

function megabytes(bytes: number): string {
    return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

/// The rows of the numbers table: a label, then this night's value and the
/// previous night's.
function rows(now: Results, before: Results | undefined): string[][] {
    const index = now.index;
    const old = before?.index;
    const sweep = now.sweep;
    const time = (seconds: number | undefined) =>
        seconds === undefined
            ? ""
            : seconds < 120
              ? `${seconds.toFixed(0)} s`
              : `${(seconds / 60).toFixed(1)} min`;
    const out: string[][] = [["prepare", time(now.prepareSeconds), time(before?.prepareSeconds)]];
    if (index !== undefined) {
        out.push(
            ["index", time(index.seconds), time(old?.seconds)],
            ["units", String(index.units), String(old?.units ?? "")],
            [
                "file shards",
                `${String(index.shards)} (${megabytes(index.shardBytes)})`,
                old === undefined ? "" : `${String(old.shards)} (${megabytes(old.shardBytes)})`,
            ],
            ["symbols", String(index.symbols), String(old?.symbols ?? "")],
            [
                "cache on disk",
                megabytes(index.cacheBytes),
                old === undefined ? "" : megabytes(old.cacheBytes),
            ],
            [
                "peak memory: master / worker / all",
                `${String(index.memory.root)} / ${String(index.memory.child)} / ${String(index.memory.total)} MB`,
                old === undefined
                    ? ""
                    : `${String(old.memory.root)} / ${String(old.memory.child)} / ${String(old.memory.total)} MB`,
            ],
            [
                "units with errors / failed",
                `${String(index.compiledWithErrors)} / ${String(index.failed)}`,
                old === undefined
                    ? ""
                    : `${String(old.compiledWithErrors)} / ${String(old.failed)}`,
            ],
        );
    }
    if (sweep !== undefined) {
        out.push(
            [
                "sampled files (server only)",
                `${String(sweep.files)} (${String(sweep.serverOnly.length)})`,
                before?.sweep === undefined
                    ? ""
                    : `${String(before.sweep.files)} (${String(before.sweep.serverOnly.length)})`,
            ],
            ["requests", String(sweep.requests), String(before?.sweep?.requests ?? "")],
            [
                "inspect / server time",
                `${time(sweep.inspectSeconds)} / ${time(sweep.serverSeconds)}`,
                before?.sweep === undefined
                    ? ""
                    : `${time(before.sweep.inspectSeconds)} / ${time(before.sweep.serverSeconds)}`,
            ],
        );
    }
    return out;
}

/// A code block of the text's first lines, its fence longer than any run
/// of backticks inside (a hover's code blocks).
function fence(text: string): string {
    const longest = Math.max(0, ...(text.match(/`+/g) ?? []).map((run) => run.length));
    const ticks = "`".repeat(Math.max(3, longest + 1));
    return [ticks, text.split("\n").slice(0, 60).join("\n"), ticks].join("\n");
}

export function report(now: Results, before?: Results): string {
    const lines = [
        `# ${now.job}: ${now.failures.length === 0 ? "green" : `${String(now.failures.length)} failures`}`,
        "",
        `${now.about}, commit \`${now.commit}\`; clice \`${now.clice}\`; sample seed \`${now.seed}\`.`,
        "",
        "| | tonight | the night before |",
        "| --- | --- | --- |",
        ...rows(now, before).map((row) => `| ${row.join(" | ")} |`),
    ];
    for (const kind of Object.keys(KINDS) as Failure["kind"][]) {
        const failures = now.failures.filter((failure) => failure.kind === kind);
        if (failures.length === 0) {
            continue;
        }
        lines.push("", `## ${KINDS[kind]} (${String(failures.length)})`);
        for (const failure of failures) {
            const where = [failure.file, failure.request]
                .filter((part) => part !== undefined && part !== "")
                .join(", ");
            lines.push(
                "",
                `### ${where === "" ? kind : where}`,
                "",
                fence(failure.detail),
                "",
                "Reproduce:",
                "",
                fence(failure.repro),
            );
        }
    }
    if (now.compilerRejects.length > 0) {
        lines.push(
            "",
            `## Units the compiler rejects too (${String(now.compilerRejects.length)})`,
            "",
        );
        lines.push(
            ...now.compilerRejects.map(
                (unit) => `- \`${unit.unit}\`: ${unit.output.split("\n")[0] ?? ""}`,
            ),
        );
    }
    if (now.logErrors.length > 0) {
        lines.push(
            "",
            `## Error lines in clice's logs (${String(now.logErrors.length)})`,
            "",
            fence(now.logErrors.join("\n")),
        );
    }
    return `${lines.join("\n")}\n`;
}
