/// A job's results as a person reads them: what failed, each with the
/// command that reproduces it, and the numbers beside the previous night's.

import type { PeakMemory } from "./memory.ts";
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

function memory(peak: PeakMemory | undefined): string {
    return peak === undefined
        ? ""
        : `${String(peak.root)} / ${String(peak.child)} / ${String(peak.total)} MB`;
}

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
            ["peak memory: master / worker / all", memory(index.memory), memory(old?.memory)],
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
        `# ${now.job} on ${now.platform}: ${now.failures.length === 0 ? "green" : `${String(now.failures.length)} failures`}`,
        "",
        `${now.about}, commit \`${now.commit}\`; clice \`${now.clice}\`; sample seed \`${now.seed}\`.`,
        "",
        "| | tonight | the night before |",
        "| --- | --- | --- |",
        ...rows(now, before).map((row) => `| ${row.join(" | ")} |`),
    ];
    const where = (failure: Failure) =>
        [failure.file, failure.request]
            .filter((part) => part !== undefined && part !== "")
            .join(", ") || failure.kind;
    for (const kind of Object.keys(KINDS) as Failure["kind"][]) {
        const failures = now.failures.filter((failure) => failure.kind === kind);
        if (failures.length === 0) {
            continue;
        }
        lines.push("", `## ${KINDS[kind]} (${String(failures.length)})`);
        // One cause failing at many places (a clice that dies on every
        // request) is shown once, with the places it failed at.
        const byDetail = new Map<string, { first: Failure; others: Failure[] }>();
        for (const failure of failures) {
            const group = byDetail.get(failure.detail);
            if (group === undefined) {
                byDetail.set(failure.detail, { first: failure, others: [] });
            } else {
                group.others.push(failure);
            }
        }
        for (const [detail, { first, others }] of byDetail) {
            lines.push(
                "",
                `### ${where(first)}${others.length === 0 ? "" : ` and ${String(others.length)} more`}`,
            );
            if (others.length > 0) {
                lines.push(
                    "",
                    ...others.slice(0, 50).map((failure) => `- ${where(failure)}`),
                    ...(others.length > 50 ? [`- ...`] : []),
                );
            }
            lines.push("", fence(detail), "", "Reproduce:", "", fence(first.repro));
        }
    }
    const slow = [...(now.sweep?.slow ?? [])].sort((a, b) => b.seconds - a.seconds);
    if (slow.length > 0) {
        lines.push(
            "",
            `## Server requests that took over a minute (${String(slow.length)})`,
            "",
            ...slow
                .slice(0, 30)
                .map(
                    (request) =>
                        `- ${request.file}, ${request.label}: ${String(request.seconds)} s`,
                ),
            ...(slow.length > 30 ? ["- ..."] : []),
        );
    }
    if (now.accepted.length > 0) {
        lines.push(
            "",
            `## Differences the project accepts, each pinned by a case (${String(now.accepted.length)})`,
            "",
            ...now.accepted.map(
                (accepted) => `- ${accepted.file}, ${accepted.label}: tests/${accepted.case}`,
            ),
        );
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
