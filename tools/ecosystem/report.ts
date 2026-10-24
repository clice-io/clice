/// A job's results as a person reads them: what failed, the failures the
/// project knows apart, each with the command that reproduces it, and the
/// numbers beside the previous round's.

import { fence } from "../testing/docs/pages.ts";
import type { PeakMemory, Resident } from "./memory.ts";
import { tables } from "./perf/compare.ts";
import type { PerfResults } from "./perf/results.ts";
import type { FAILURE_KINDS, Results } from "./regression/regression.ts";
import type { Failure } from "./script.ts";

const KINDS: Record<(typeof FAILURE_KINDS)[number], string> = {
    setup: "The project could not be prepared",
    crash: "Crashes",
    hang: "Hangs",
    unfinished: "Runs of clice index that did not finish",
    index: "Units that failed to index, which the compiler compiles",
    memory: "Units clice gave up as too large for the machine",
    compile: "Units clice reports errors on, which the compiler compiles",
    disagree: "Places where inspect and the server answer differently",
    inspect: "Requests clice inspect failed",
    server: "Requests the server failed",
    anomaly: "Anomalies",
    runner: "Errors of the runner itself",
};

function memory(peak: PeakMemory | undefined): string {
    return peak === undefined
        ? ""
        : `${String(peak.root)} / ${String(peak.child)} / ${String(peak.total)} MB`;
}

function resident(measured: { start: Resident; end: Resident } | undefined): string {
    return measured === undefined
        ? ""
        : `${String(measured.start.root)} + ${String(measured.start.children)} / ${String(measured.end.root)} + ${String(measured.end.children)} MB`;
}

function megabytes(bytes: number): string {
    return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function time(seconds: number | undefined): string {
    return seconds === undefined
        ? ""
        : seconds < 120
          ? `${seconds.toFixed(0)} s`
          : `${(seconds / 60).toFixed(1)} min`;
}

/// A job's numbers as the table shows them, each under its row's label.
function numbers(results: Results): Map<string, string> {
    const { index, sweep } = results;
    const rows: [string, string][] = [["prepare", time(results.prepareSeconds)]];
    if (index !== undefined) {
        rows.push(
            ["index", time(index.seconds)],
            ["units", String(index.units)],
            ["file shards", `${String(index.shards)} (${megabytes(index.shardBytes)})`],
            ["symbols", String(index.symbols)],
            ["cache on disk", megabytes(index.cacheBytes)],
            ["peak memory: master / worker / all", memory(index.memory)],
            [
                "units with errors / failed",
                `${String(index.compiledWithErrors)} / ${String(index.failed)}`,
            ],
        );
    }
    if (sweep !== undefined) {
        rows.push(
            [
                "sampled files (server only)",
                `${String(sweep.files)} (${String(sweep.serverOnly.length)})`,
            ],
            ["requests", String(sweep.requests)],
            ["server memory: master + workers, at start / once all closed", resident(sweep.memory)],
            ["server peak memory: master / worker / all", memory(sweep.memory?.peak)],
            [
                "inspect / server time",
                `${time(sweep.inspectSeconds)} / ${time(sweep.serverSeconds)}`,
            ],
        );
    }
    return new Map(rows);
}

function listed(items: readonly string[], limit: number): string[] {
    return [...items.slice(0, limit), ...(items.length > limit ? ["- ..."] : [])];
}

/// A code block of the text's first lines.
function codeBlock(text: string): string {
    return fence(text.split("\n").slice(0, 60).join("\n"), "").join("\n");
}

function where(failure: Failure): string {
    const request = `${failure.request ?? ""}${failure.at === undefined ? "" : ` at \`${failure.at}\``}`;
    return (
        [failure.file, request].filter((part) => part !== undefined && part !== "").join(", ") ||
        failure.kind
    );
}

/// A section per kind of failure, in the order `kinds` lists them: one
/// cause failing at many places (a clice that dies on every request) is
/// shown once, with the places it failed at.
function failureSections(failures: readonly Failure[], kinds: Record<string, string>): string[] {
    const lines: string[] = [];
    const order = [...Object.keys(kinds), ...failures.map((failure) => failure.kind)];
    for (const kind of new Set(order)) {
        const ofKind = failures.filter((failure) => failure.kind === kind);
        if (ofKind.length === 0) {
            continue;
        }
        lines.push("", `## ${kinds[kind] ?? kind} (${String(ofKind.length)})`);
        const byDetail = new Map<string, { first: Failure; others: Failure[] }>();
        for (const failure of ofKind) {
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
                    ...listed(
                        others.map((failure) => `- ${where(failure)}`),
                        50,
                    ),
                );
            }
            lines.push("", codeBlock(detail), "", "Reproduce:", "", codeBlock(first.repro));
        }
    }
    return lines;
}

/// The failures the project knows, by issue, and the known issues the job
/// did not see.
function knownSections(
    known: readonly (Failure & { issue: string })[],
    unseen: readonly { issue: string; why: string }[],
): string[] {
    const lines: string[] = [];
    if (known.length > 0) {
        lines.push("", `## Known failures (${String(known.length)})`);
        for (const issue of new Set(known.map((failure) => failure.issue))) {
            const ofIssue = known.filter((failure) => failure.issue === issue);
            lines.push(
                "",
                `### ${issue}: ${where(ofIssue[0] ?? { kind: "", detail: "", repro: "" })}${ofIssue.length === 1 ? "" : ` and ${String(ofIssue.length - 1)} more`}`,
                "",
                codeBlock(ofIssue[0]?.detail ?? ""),
            );
        }
    }
    if (unseen.length > 0) {
        lines.push(
            "",
            `## Known issues not seen this round (${String(unseen.length)})`,
            "",
            ...unseen.map((known) => `- ${known.issue}: ${known.why}`),
        );
    }
    return lines;
}

function verdict(fresh: number, known: number): string {
    const failures = fresh === 0 ? "green" : `${String(fresh)} new failures`;
    return known === 0 ? failures : `${failures}, ${String(known)} known`;
}

export function regressionReport(now: Results, before?: Results): string {
    const old = before === undefined ? new Map<string, string>() : numbers(before);
    const lines = [
        `# regression ${now.job} on ${now.platform}: ${verdict(now.failures.length, now.known.length)}`,
        "",
        `${now.about}, commit \`${now.commit}\`; clice \`${now.clice}\`; sample seed \`${now.seed}\`.`,
        "",
        "| | this round | the round before |",
        "| --- | --- | --- |",
        ...[...numbers(now)].map(
            ([label, value]) => `| ${label} | ${value} | ${old.get(label) ?? ""} |`,
        ),
        ...failureSections(now.failures, KINDS),
        ...knownSections(now.known, now.unseen),
    ];
    const section = (title: string, count: number, body: string[]) => {
        if (count > 0) {
            lines.push("", `## ${title} (${String(count)})`, "", ...body);
        }
    };
    const slow = [...(now.sweep?.slow ?? [])].sort((a, b) => b.seconds - a.seconds);
    section(
        "Server requests that took over a minute",
        slow.length,
        listed(
            slow.map(
                (request) => `- ${request.file}, ${request.label}: ${String(request.seconds)} s`,
            ),
            30,
        ),
    );
    section(
        "Differences the project accepts, each pinned by a case",
        now.accepted.length,
        now.accepted.map((each) => `- ${each.file}, ${each.label}: tests/${each.case}`),
    );
    section(
        "Units the compiler rejects too",
        now.compilerRejects.length,
        now.compilerRejects.map(
            (unit) => `- \`${unit.unit}\`: ${unit.output.split("\n")[0] ?? ""}`,
        ),
    );
    section("Error lines in clice's logs", now.logErrors.length, [
        codeBlock(now.logErrors.join("\n")),
    ]);
    return `${lines.join("\n")}\n`;
}

const PERF_KINDS: Record<string, string> = {
    setup: "The project could not be prepared",
    crash: "Runs clice crashed in",
    hang: "Runs clice stopped answering in",
    server: "Runs in which clice answered with an error",
    unfinished: "Scenarios the job's time ran out before",
    runner: "Errors of the runner itself",
};

export function perfReport(now: PerfResults): string {
    const lines = [
        `# perf ${now.job} on ${now.platform}: ${verdict(now.failures.length, now.known.length)}`,
        "",
        `${now.about}, commit \`${now.commit}\`; clice \`${now.clice}\`.`,
        ...failureSections(now.failures, PERF_KINDS),
        ...knownSections(now.known, now.unseen),
        "",
        ...tables([now]),
    ];
    return `${lines.join("\n")}\n`;
}
