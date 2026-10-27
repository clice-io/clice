/// A round of the ecosystem workflow read in one place: every job's new
/// failures, each beside whether earlier rounds swept its file, the
/// regression numbers that moved since the round before, and every perf
/// measure past its threshold by ratio, with what makes it suspect. The
/// workflow's last job writes it as the round's summary; here it also
/// downloads the logs of what it flags (the ecosystem-round skill).
///
/// Usage:
///   node tools/ecosystem/round.ts <run id>|latest [options]
///
/// Options:
///   --out <dir>        where the round goes (default temp/ecosystem/<run id>):
///                      round.md, round.json, results/<artifact>/ of every
///                      job, logs/<artifact>/ of the flagged ones
///   --history <n>      the rounds on main before it to compare with (default 14)
///   --no-logs          leave the flagged jobs' logs undownloaded
///
/// `latest` is the newest finished round on main. gh must be logged in, or
/// GH_TOKEN set.

import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { promisify } from "node:util";
import { parseArgs } from "node:util";
import { REPO_ROOT } from "../repo.ts";
import type { PerfResults, RunResult, Slower } from "./perf/results.ts";
import { PLATFORMS, type Platform } from "./project.ts";
import type { Results } from "./regression/regression.ts";
import { artifactName, type Failure, type JobKind } from "./script.ts";

/// One job of a round: its results, or GitHub's word on why it has none.
export interface RoundJob {
    kind: JobKind;
    name: string;
    platform: Platform;
    artifact: string;
    conclusion: string;
    regression?: Results;
    perf?: PerfResults;
}

/// A regression job's results in a round before.
export interface Earlier {
    run: number;
    date: string;
    results: Results;
}

export interface Round {
    run: number;
    jobs: RoundJob[];
    /// By artifact, the job's results in the rounds before, newest first.
    earlier: ReadonlyMap<string, readonly Earlier[]>;
}

/// A measure past its threshold, and what makes it suspect.
export interface PerfRow extends Slower {
    job: string;
    platform: Platform;
    artifact: string;
    flags: string[];
}

export interface RoundData {
    run: number;
    clice: string | undefined;
    baseline: string | undefined;
    seed: string | undefined;
    red: {
        artifact: string;
        kind: JobKind;
        job: string;
        platform: Platform;
        failures: (Failure & { history?: string })[];
    }[];
    /// The jobs without results, and those that failed with results naming
    /// no failure (`partial`).
    missing: {
        artifact: string;
        kind: JobKind;
        job: string;
        platform: Platform;
        conclusion: string;
        partial: boolean;
    }[];
    moved: {
        job: string;
        platform: Platform;
        measure: string;
        now: number;
        before: number;
        ratio: number;
    }[];
    broad: string[];
    perf: PerfRow[];
    /// The artifacts whose logs a reader wants first.
    flagged: string[];
}

/// A perf measure this many times the baseline's is more than a day's
/// commits change: the two servers likely did different work.
const EXTREME = 3;
/// A regression number this many times the round before's is listed.
const MOVED = 1.5;

function failureOf(failure: Failure): string {
    const at = failure.at === undefined ? "" : ` at \`${failure.at}\``;
    const where = [failure.file, failure.request === undefined ? "" : `${failure.request}${at}`]
        .filter((part) => part !== undefined && part !== "")
        .join(", ");
    return where === "" ? failure.kind : where;
}

function sameFailure(a: Failure, b: Failure): boolean {
    return a.kind === b.kind && a.file === b.file && a.request === b.request;
}

/// Whether the rounds before swept a failing file, and how it did there: a
/// file newly sampled points at an old bug the sample exposed, one that
/// passed before at a regression. A failure outside the sweep (the index,
/// the setup) runs every round alike.
export function historyOf(failure: Failure, earlier: readonly Earlier[]): string | undefined {
    if (failure.file === undefined || earlier.length === 0) {
        return undefined;
    }
    const failed = (each: Earlier) =>
        [...each.results.failures, ...each.results.known].some((other) =>
            sameFailure(failure, other),
        );
    const before = earlier.filter(failed);
    const last = before[0];
    if (last !== undefined) {
        return `failed so in ${String(before.length)} of the ${String(earlier.length)} rounds before, last ${last.date}`;
    }
    if (failure.request === undefined) {
        return `new: none of the ${String(earlier.length)} rounds before failed so`;
    }
    const recorded = earlier.filter((each) => Array.isArray(each.results.sweep?.files));
    if (recorded.length === 0) {
        return `unknown: the ${String(earlier.length)} rounds before recorded no swept files`;
    }
    const swept = recorded.filter((each) => each.results.sweep?.files.includes(failure.file ?? ""));
    const lastSwept = swept[0];
    return lastSwept === undefined
        ? `newly sampled: none of the ${String(recorded.length)} rounds before swept it, likely an old bug; run the reproduce command with the previous nightly`
        : `swept in ${String(swept.length)} of the ${String(recorded.length)} rounds before (last ${lastSwept.date}), never failing so: a regression suspect`;
}

interface Moved {
    measure: string;
    of: (results: Results) => number | undefined;
}

const MOVES: Moved[] = [
    { measure: "index s", of: (r) => r.index?.seconds },
    { measure: "index peak memory MB", of: (r) => r.index?.memory?.total },
    { measure: "sweep: inspect s", of: (r) => r.sweep?.inspectSeconds },
    { measure: "sweep: server s", of: (r) => r.sweep?.serverSeconds },
    { measure: "sweep: server peak memory MB", of: (r) => r.sweep?.memory?.peak.total },
    {
        measure: "sweep: server memory once all closed MB",
        of: (r) => {
            const end = r.sweep?.memory?.end;
            return end === undefined ? undefined : end.root + end.children;
        },
    },
];

/// The baseline nightly's tag, as the job's path to it names it.
function baselineOf(perf: PerfResults): string | undefined {
    const binary = perf.binaries.baseline;
    return binary === undefined
        ? undefined
        : (/baseline[\\/]([^\\/]+)[\\/]/.exec(binary)?.[1] ?? binary);
}

function median(values: readonly number[]): number | undefined {
    const sorted = [...values].sort((a, b) => a - b);
    return sorted.length === 0 ? undefined : sorted[Math.floor((sorted.length - 1) / 2)];
}

/// A row's measure without the variant its column names: `open (no PCH)`
/// is `open`.
function baseMeasure(measure: string): string {
    return measure.replace(/ \([^)]+\)$/, "");
}

/// The runs a row's medians come from on one side.
function runsOf(perf: PerfResults, row: Slower, server: "clice" | "baseline"): RunResult[] {
    const variant = /\(([^)]+)\)$/.exec(row.measure)?.[1];
    return perf.runs.filter(
        (run) =>
            run.server === server &&
            run.error === undefined &&
            run.variant === variant &&
            `${run.scenario} ${run.label}` === row.name,
    );
}

/// Whether a time compares one sample of each side: a cold start's varies
/// severalfold between two runs on one machine.
function oneSample(perf: PerfResults, row: Slower): boolean {
    const measure = baseMeasure(row.measure);
    const samples = (server: "clice" | "baseline") =>
        runsOf(perf, row, server).reduce((sum, run) => sum + (run.series[measure]?.length ?? 1), 0);
    return !measure.includes("MB") && samples("clice") <= 1 && samples("baseline") <= 1;
}

/// For a memory row, the process whose share differs most between the two
/// servers: a worker holding hundreds of megabytes on one side alone did
/// work the other did not.
function roleGap(perf: PerfResults, row: Slower): string | undefined {
    const measure = baseMeasure(row.measure);
    const snapshot = /^memory (.+) MB$/.exec(measure)?.[1];
    if (measure !== "peak memory MB" && snapshot === undefined) {
        return undefined;
    }
    const rolesOf = (run: RunResult) =>
        snapshot === undefined ? run.memory?.roles : run.snapshots[snapshot]?.roles;
    const medians = (server: "clice" | "baseline") => {
        const byRole = new Map<string, number[]>();
        for (const roles of runsOf(perf, row, server).map(rolesOf)) {
            for (const [role, mb] of Object.entries(roles ?? {})) {
                byRole.set(role, [...(byRole.get(role) ?? []), mb]);
            }
        }
        return new Map([...byRole].map(([role, values]) => [role, median(values) ?? 0]));
    };
    const clice = medians("clice");
    const baseline = medians("baseline");
    let widest: { role: string; gap: number } | undefined;
    for (const role of new Set([...clice.keys(), ...baseline.keys()])) {
        const gap = Math.abs((clice.get(role) ?? 0) - (baseline.get(role) ?? 0));
        if (widest === undefined || gap > widest.gap) {
            widest = { role, gap };
        }
    }
    return widest === undefined
        ? undefined
        : `${widest.role} ${String(clice.get(widest.role) ?? 0)} vs ${String(baseline.get(widest.role) ?? 0)} MB`;
}

/// The measures past their threshold on so many jobs at once, across
/// projects, that the measurement is the likelier cause than the code.
function broadMeasures(perfs: readonly RoundJob[]): Map<string, string> {
    const measured = perfs.filter((job) => job.perf?.binaries.baseline !== undefined);
    const byMeasure = new Map<string, RoundJob[]>();
    for (const job of measured) {
        const bases = new Set((job.perf?.slower ?? []).map((row) => baseMeasure(row.measure)));
        for (const base of bases) {
            byMeasure.set(base, [...(byMeasure.get(base) ?? []), job]);
        }
    }
    const broad = new Map<string, string>();
    for (const [measure, jobs] of byMeasure) {
        const projects = new Set(jobs.map((job) => job.name.split("/")[0]));
        if (jobs.length >= 4 && jobs.length * 3 >= measured.length && projects.size >= 2) {
            const platforms = PLATFORMS.filter((platform) =>
                jobs.some((job) => job.platform === platform),
            );
            broad.set(
                measure,
                `\`${measure}\` is past the threshold on ${String(jobs.length)} of the ${String(measured.length)} perf jobs (${platforms.join(", ")}): a change on most jobs at once points at the measurement (one sample a side, the run order, the file cache) before the code`,
            );
        }
    }
    return broad;
}

function perfRows(jobs: readonly RoundJob[], broad: ReadonlyMap<string, string>): PerfRow[] {
    const perfs = jobs.filter((job) => job.perf !== undefined);
    const rows: PerfRow[] = [];
    for (const job of perfs) {
        const perf = job.perf;
        if (perf === undefined) {
            continue;
        }
        const platforms = perfs.filter((other) => other.name === job.name);
        for (const row of perf.slower) {
            const flags: string[] = [];
            const on = platforms.filter((other) =>
                (other.perf?.slower ?? []).some(
                    (each) => each.name === row.name && each.measure === row.measure,
                ),
            );
            if (platforms.length > 1) {
                flags.push(
                    on.length === 1
                        ? `${job.platform} only`
                        : on.length === platforms.length
                          ? "every platform"
                          : `on ${on.map((other) => other.platform).join(", ")}`,
                );
            }
            if ([row.clice, row.baseline].some((value) => Number.parseFloat(value) < 0)) {
                flags.push("negative: the harness measured wrong");
            } else if (row.clicePerBaseline >= EXTREME) {
                flags.push(`≥${String(EXTREME)}×: likely different work on the two sides`);
            }
            if (oneSample(perf, row)) {
                flags.push("one sample a side");
            }
            const gap = roleGap(perf, row);
            if (gap !== undefined) {
                flags.push(gap);
            }
            if (broad.has(baseMeasure(row.measure))) {
                flags.push("broad");
            }
            rows.push({
                ...row,
                job: job.name,
                platform: job.platform,
                artifact: job.artifact,
                flags,
            });
        }
    }
    return rows.sort((a, b) => b.clicePerBaseline - a.clicePerBaseline);
}

function shown(value: number): string {
    return value >= 100 ? value.toFixed(0) : value.toFixed(1);
}

function cell(text: string): string {
    return text.replaceAll("|", "\\|").replaceAll("\n", " ");
}

export function roundReport(round: Round): { text: string; data: RoundData } {
    const regressions = round.jobs.filter((job) => job.regression !== undefined);
    const perfs = round.jobs.filter((job) => job.perf !== undefined);
    const firstPerf = perfs.find((job) => job.perf?.binaries.baseline !== undefined)?.perf;
    const red = round.jobs.flatMap((job) => {
        const failures = job.regression?.failures ?? job.perf?.failures ?? [];
        const earlier = round.earlier.get(job.artifact) ?? [];
        return failures.length === 0
            ? []
            : [
                  {
                      artifact: job.artifact,
                      kind: job.kind,
                      job: job.name,
                      platform: job.platform,
                      failures: failures.map((failure): Failure & { history?: string } => {
                          const history =
                              job.kind === "regression" ? historyOf(failure, earlier) : undefined;
                          return history === undefined ? failure : { ...failure, history };
                      }),
                  },
              ];
    });
    // A job cut short (its runner killed, its time out) can leave the
    // results a perf job saves as it goes, which name no failure yet.
    const missing = round.jobs
        .filter(
            (job) =>
                (job.regression ?? job.perf) === undefined ||
                (job.conclusion !== "success" &&
                    (job.regression ?? job.perf)?.failures.length === 0),
        )
        .map(({ kind, name, platform, artifact, conclusion, regression, perf }) => ({
            artifact,
            kind,
            job: name,
            platform,
            conclusion,
            partial: (regression ?? perf) !== undefined,
        }));
    const moved = regressions.flatMap((job) => {
        const before = round.earlier.get(job.artifact)?.[0]?.results;
        const now = job.regression;
        if (before === undefined || now === undefined) {
            return [];
        }
        return MOVES.flatMap(({ measure, of }) => {
            const [a, b] = [of(now), of(before)];
            return a === undefined || b === undefined || b <= 0 || a / b < MOVED
                ? []
                : [
                      {
                          job: job.name,
                          platform: job.platform,
                          measure,
                          now: a,
                          before: b,
                          ratio: a / b,
                      },
                  ];
        });
    });
    moved.sort((a, b) => b.ratio - a.ratio);
    const broad = broadMeasures(perfs);
    const rows = perfRows(round.jobs, broad);
    const flagged = [
        ...new Set([
            ...red.map((job) => job.artifact),
            ...missing.filter((job) => job.partial).map((job) => job.artifact),
            ...rows
                .filter((row) =>
                    row.flags.some((flag) => /^(every platform|negative|≥)/.test(flag)),
                )
                .map((row) => row.artifact),
        ]),
    ];
    const data: RoundData = {
        run: round.run,
        clice: (regressions[0]?.regression ?? perfs[0]?.perf)?.clice,
        baseline: firstPerf === undefined ? undefined : baselineOf(firstPerf),
        seed: regressions[0]?.regression?.seed,
        red,
        missing,
        moved,
        broad: [...broad.values()],
        perf: rows,
        flagged,
    };

    const lines = [
        `# ecosystem round ${String(round.run)}: ${red.length === 0 && missing.length === 0 ? "green" : `${String(red.length)} red, ${String(missing.length)} cut short or without results`}, of ${String(round.jobs.length)} jobs`,
        "",
        [
            data.clice === undefined ? undefined : `clice \`${data.clice}\``,
            data.baseline === undefined
                ? perfs.length > 0
                    ? "perf against clangd"
                    : undefined
                : `perf against the nightly \`${data.baseline}\``,
            data.seed === undefined ? undefined : `sample seed \`${data.seed}\``,
        ]
            .filter((part) => part !== undefined)
            .join("; ") + ". Each job's own report: `results/<artifact>/report.md`.",
    ];
    if (red.length > 0) {
        lines.push("", `## Red jobs (${String(red.length)})`);
        for (const job of red) {
            lines.push(
                "",
                `### ${job.kind} ${job.job} on ${job.platform}: ${String(job.failures.length)} new failure${job.failures.length === 1 ? "" : "s"}`,
                "",
                `\`${job.artifact}\``,
                "",
            );
            for (const failure of job.failures.slice(0, 20)) {
                // A diff's headers say nothing: its first changed line does.
                const detail =
                    failure.detail
                        .split("\n")
                        .find(
                            (line) => line.trim() !== "" && !/^(=+$|--- |\+\+\+ |@@ )/.test(line),
                        ) ?? "";
                lines.push(`- **${failure.kind}** ${failureOf(failure)}: ${detail.slice(0, 200)}`);
                if (failure.history !== undefined) {
                    lines.push(`  - ${failure.history}`);
                }
                lines.push(`  - reproduce: \`${failure.repro}\``);
            }
            if (job.failures.length > 20) {
                lines.push(`- and ${String(job.failures.length - 20)} more: the job's report`);
            }
        }
    }
    if (missing.length > 0) {
        lines.push(
            "",
            `## Jobs without results, or cut short (${String(missing.length)})`,
            "",
            ...missing.map(
                (job) =>
                    `- ${job.kind} ${job.job} on ${job.platform}: the job's conclusion: ${job.conclusion}${job.partial ? ", its results name no failure: cut short, see its log" : ""}`,
            ),
        );
    }
    if (moved.length > 0) {
        lines.push(
            "",
            `## Regression numbers ${String(MOVED)}× the round before's or more (${String(moved.length)})`,
            "",
            "The sweep's numbers depend on the files it samples, which change every round.",
            "",
            "| job | platform | measure | this round | the round before | ratio |",
            "|---|---|---|---:|---:|---:|",
            ...moved.map(
                (each) =>
                    `| ${each.job} | ${each.platform} | ${each.measure} | ${shown(each.now)} | ${shown(each.before)} | ${each.ratio.toFixed(2)}× |`,
            ),
        );
    }
    if (rows.length > 0) {
        lines.push("", `## Perf past the threshold (${String(rows.length)}), by ratio`, "");
        if (broad.size > 0) {
            lines.push(...[...broad.values()].map((text) => `- ${text}`), "");
        }
        lines.push(
            "| job | platform | scenario | measure | clice | baseline | ratio | flags |",
            "|---|---|---|---|---:|---:|---:|---|",
            ...rows.map(
                (row) =>
                    `| ${row.job} | ${row.platform} | ${cell(row.name)} | ${cell(row.measure)} | ${row.clice} | ${row.baseline} | ${row.clicePerBaseline.toFixed(2)}× | ${cell(row.flags.join("; "))} |`,
            ),
        );
    }
    if (flagged.length > 0) {
        lines.push(
            "",
            "## Flagged: their logs first",
            "",
            ...flagged.map((artifact) => `- \`ecosystem-${artifact}-logs\``),
        );
    }
    return { text: `${lines.join("\n")}\n`, data };
}

const exec = promisify(execFile);

async function gh(args: readonly string[]): Promise<string> {
    const { stdout } = await exec("gh", args, { maxBuffer: 256 * 1024 * 1024, cwd: REPO_ROOT });
    return stdout;
}

/// The run's artifacts, each into `dir/<name>` (gh unpacks a single one
/// into the directory itself).
async function download(run: number, names: readonly string[], dir: string): Promise<void> {
    if (names.length === 0) {
        return;
    }
    fs.mkdirSync(dir, { recursive: true });
    const target = names.length === 1 ? path.join(dir, names[0] ?? "") : dir;
    await gh([
        "run",
        "download",
        String(run),
        "-D",
        target,
        ...names.flatMap((name) => ["-n", name]),
    ]);
}

async function artifactsOf(run: number): Promise<string[]> {
    const text = await gh([
        "api",
        "--paginate",
        `repos/{owner}/{repo}/actions/runs/${String(run)}/artifacts`,
        "--jq",
        ".artifacts[] | select(.expired | not) | .name",
    ]);
    return text.split("\n").filter((name) => name !== "");
}

function readJson(file: string): unknown {
    return fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, "utf8")) : undefined;
}

/// The jobs of a round, as GitHub names them: `<kind> <job> (<platform>)`.
const JOB_NAME = /^(regression|perf) (\S+) \((linux|macos|windows)\)$/;

async function main(): Promise<void> {
    const { values, positionals } = parseArgs({
        options: {
            out: { type: "string" },
            history: { type: "string", default: "14" },
            "no-logs": { type: "boolean", default: false },
        },
        allowPositionals: true,
    });
    const [which] = positionals;
    if (which === undefined) {
        console.error("usage: node tools/ecosystem/round.ts <run id>|latest [--out <dir>]");
        process.exit(2);
    }
    const rounds = JSON.parse(
        await gh([
            "run",
            "list",
            "--workflow",
            "ecosystem.yml",
            "--branch",
            "main",
            "--status",
            "completed",
            "--limit",
            "60",
            "--json",
            "databaseId,createdAt",
        ]),
    ) as { databaseId: number; createdAt: string }[];
    const run = which === "latest" ? rounds[0]?.databaseId : Number(which);
    if (run === undefined || Number.isNaN(run)) {
        console.error(`no round: ${which}`);
        process.exit(2);
    }
    const view = JSON.parse(await gh(["run", "view", String(run), "--json", "createdAt,jobs"])) as {
        createdAt: string;
        jobs: { name: string; conclusion: string; status: string }[];
    };
    const out = path.resolve(values.out ?? path.join(REPO_ROOT, "temp", "ecosystem", String(run)));
    // What an earlier run wrote there, and nothing else the directory holds.
    for (const written of ["round.md", "round.json", "results", "earlier", "logs"]) {
        fs.rmSync(path.join(out, written), { recursive: true, force: true });
    }
    fs.mkdirSync(out, { recursive: true });

    const listed = view.jobs.flatMap((job) => {
        const match = JOB_NAME.exec(job.name);
        if (match === null) {
            return [];
        }
        const [, kind, name, platform] = match as unknown as [string, JobKind, string, Platform];
        return [
            {
                kind,
                name,
                platform,
                artifact: artifactName(kind, name, platform),
                conclusion: job.conclusion === "" ? job.status : job.conclusion,
            },
        ];
    });
    const artifacts = new Set(await artifactsOf(run));
    const results = path.join(out, "results");
    await download(
        run,
        listed.map((job) => `ecosystem-${job.artifact}`).filter((name) => artifacts.has(name)),
        results,
    );
    const jobs: RoundJob[] = listed.map((job) => {
        const file = path.join(results, `ecosystem-${job.artifact}`, "results.json");
        const regression =
            job.kind === "regression" ? (readJson(file) as Results | undefined) : undefined;
        const perf = job.kind === "perf" ? (readJson(file) as PerfResults | undefined) : undefined;
        return {
            ...job,
            ...(regression === undefined ? {} : { regression }),
            ...(perf === undefined ? {} : { perf }),
        };
    });

    // The round before every regression job's numbers, the rounds before
    // that for the red jobs' sampled files.
    const before = rounds
        .filter((each) => each.createdAt < view.createdAt && each.databaseId !== run)
        .slice(0, Number(values.history));
    const red = new Set(
        jobs.filter((job) => (job.regression?.failures.length ?? 0) > 0).map((job) => job.artifact),
    );
    const earlier = new Map<string, Earlier[]>();
    for (const [i, { databaseId, createdAt }] of before.entries()) {
        const wanted = jobs
            .filter((job) => job.kind === "regression" && (i === 0 || red.has(job.artifact)))
            .map((job) => `ecosystem-${job.artifact}`);
        const present = new Set(await artifactsOf(databaseId));
        const dir = path.join(out, "earlier", String(databaseId));
        await download(
            databaseId,
            wanted.filter((name) => present.has(name)),
            dir,
        );
        for (const name of wanted) {
            const found = readJson(path.join(dir, name, "results.json")) as Results | undefined;
            if (found !== undefined) {
                const artifact = name.slice("ecosystem-".length);
                earlier.set(artifact, [
                    ...(earlier.get(artifact) ?? []),
                    { run: databaseId, date: createdAt.slice(0, 10), results: found },
                ]);
            }
        }
    }

    const { text, data } = roundReport({ run, jobs, earlier });
    fs.writeFileSync(path.join(out, "round.md"), text);
    fs.writeFileSync(path.join(out, "round.json"), `${JSON.stringify(data, null, 1)}\n`);
    if (!values["no-logs"]) {
        for (const artifact of data.flagged) {
            const name = `ecosystem-${artifact}-logs`;
            if (artifacts.has(name)) {
                await download(run, [name], path.join(out, "logs"));
            }
        }
    }
    console.log(
        `${path.join(out, "round.md")}: ${String(data.red.length)} red, ${String(data.missing.length)} cut short or without results, ${String(data.perf.length)} perf rows past the threshold, ${String(data.flagged.length)} flagged`,
    );
}

if (import.meta.main) {
    await main();
}
