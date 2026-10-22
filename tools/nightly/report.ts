/// A job's results as a person reads them: what failed, each with the
/// command that reproduces it, and the numbers beside the previous night's.

import { fence } from "../testing/docs/pages.ts";
import type { PeakMemory, Resident } from "./memory.ts";
import type { Failure, Results } from "./nightly.ts";

const KINDS: Record<Failure["kind"], string> = {
    setup: "The project could not be prepared",
    crash: "Crashes",
    hang: "Hangs",
    unfinished: "Runs of clice index that did not finish",
    index: "Units that failed to index, which the compiler compiles",
    memory: "Units too large to index on the runner",
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

export function report(now: Results, before?: Results): string {
    const old = before === undefined ? new Map<string, string>() : numbers(before);
    const lines = [
        `# ${now.job} on ${now.platform}: ${now.failures.length === 0 ? "green" : `${String(now.failures.length)} failures`}`,
        "",
        `${now.about}, commit \`${now.commit}\`; clice \`${now.clice}\`; sample seed \`${now.seed}\`.`,
        "",
        "| | tonight | the night before |",
        "| --- | --- | --- |",
        ...[...numbers(now)].map(
            ([label, value]) => `| ${label} | ${value} | ${old.get(label) ?? ""} |`,
        ),
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
                    ...listed(
                        others.map((failure) => `- ${where(failure)}`),
                        50,
                    ),
                );
            }
            lines.push("", codeBlock(detail), "", "Reproduce:", "", codeBlock(first.repro));
        }
    }
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
