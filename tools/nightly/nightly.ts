/// One job of the nightly run: prepare the project, index it from a cold
/// cache, confirm with the compiler every unit clice reported errors on,
/// ask the features on sampled files, and collect what failed with how to
/// reproduce it. Counts, sizes, memory and times are recorded, never judged.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { entrySource, type DatabaseEntry } from "../compat/database.ts";
import { REPO_ROOT } from "../repo.ts";
import { logFiles } from "../process_gate.ts";
import { levelOf } from "../testing/logs/logs.ts";
import { CRASHED_IN } from "../testing/server/serve.ts";
import { confirm } from "./confirm.ts";
import { jobDatabase } from "./database.ts";
import { runIndex } from "./index.ts";
import type { PeakMemory } from "./memory.ts";
import { prepare } from "./prepare.ts";
import type { Job, Platform } from "./projects.ts";
import { sampleFiles, sweep, type Request } from "./sweep.ts";

export type Step = "index" | "sweep";

/// No file is swept after this long into the sweep, nor this long into the
/// job: the files in flight finish and the results upload within the
/// runner's six hours, after an index run that took its full bound.
const SWEEP_MS = 2 * 60 * 60_000;
const JOB_MS = 270 * 60_000;

export interface Failure {
    kind:
        | "setup"
        | "crash"
        | "hang"
        | "unfinished"
        | "index"
        | "compile"
        | "disagree"
        | "inspect"
        | "server"
        | "anomaly"
        | "runner";
    file?: string;
    request?: string;
    detail: string;
    /// The command that reproduces it, run from the repository root.
    repro: string;
}

export interface Results {
    job: string;
    platform: Platform;
    about: string;
    /// The project's pinned commit, and clice's.
    commit: string;
    clice: string;
    seed: string;
    prepareSeconds?: number;
    index?: {
        seconds: number;
        units: number;
        shards: number;
        shardBytes: number;
        symbols: number;
        cacheBytes: number;
        /// Measured on Linux only.
        memory?: PeakMemory;
        compiledWithErrors: number;
        failed: number;
    };
    sweep?: {
        files: number;
        serverOnly: { file: string; why: string }[];
        requests: number;
        inspectSeconds: number;
        serverSeconds: number;
        /// The requests that took over a minute.
        slow: { file: string; label: string; side: "inspect" | "server"; seconds: number }[];
    };
    /// Units clice reported errors on that the compiler rejects too.
    compilerRejects: { unit: string; output: string }[];
    /// Error-level lines of clice's logs: what went wrong without failing.
    logErrors: string[];
    failures: Failure[];
    /// The places whose answers differ as the project accepts, each with the
    /// case pinning it.
    accepted: { file: string; label: string; case: string }[];
}

export interface NightlyOptions {
    job: Job;
    platform: Platform;
    /// Where projects are checked out.
    dir: string;
    /// The clice under test, as reproduction commands name it; the job runs
    /// it through CLICE_EXECUTABLE.
    clice: string;
    steps: ReadonlySet<Step>;
    seed: string;
    /// Sweep these checkout-relative files instead of a sample.
    files?: string[];
    only?: ReadonlySet<Request>;
}

/// The command running one step of this job again.
function rerun(options: NightlyOptions, extra: string): string {
    return `CLICE_EXECUTABLE=${options.clice} pixi run node tools/nightly/run.ts ${options.job.name} --dir ${options.dir} ${extra}`;
}

function cliceCommit(): string {
    return execFileSync("git", ["rev-parse", "HEAD"], { cwd: REPO_ROOT, encoding: "utf8" }).trim();
}

/// The checkout-relative path of a unit inside the checkout; null for one
/// outside it.
function inside(checkout: string, file: string): string | null {
    const real = fs.existsSync(file) ? fs.realpathSync(file) : file;
    const relative = path.relative(fs.realpathSync(checkout), real);
    return relative.startsWith("..") || path.isAbsolute(relative)
        ? null
        : relative.split(path.sep).join("/");
}

/// Crashes and anomalies in the logs of every clice process the job ran
/// fail it; the other error-level lines are recorded. The logs of earlier
/// runs (`earlier`) are not this job's.
function scanLogs(
    checkout: string,
    earlier: ReadonlySet<string>,
    results: Results,
    options: NightlyOptions,
): void {
    const lines = logFiles(checkout)
        .filter((file) => !earlier.has(file))
        .flatMap((file) =>
            fs
                .readFileSync(file, "utf8")
                .split(/\r?\n/)
                .map((text) => ({ file: path.basename(file), text })),
        );
    for (const line of lines) {
        const crashed = CRASHED_IN.exec(line.text);
        if (crashed !== null) {
            results.failures.push({
                kind: "crash",
                request: crashed[1] ?? "",
                file: crashed[2] ?? "",
                detail: `${line.file}: ${line.text}`,
                repro: rerun(
                    options,
                    line.file === "index.log"
                        ? "--steps index"
                        : `--steps sweep --file ${inside(checkout, crashed[2] ?? "") ?? ""}`,
                ),
            });
        } else if (/\[anomaly:\w+\]/.test(line.text)) {
            results.failures.push({
                kind: "anomaly",
                detail: `${line.file}: ${line.text}`,
                repro: rerun(options, ""),
            });
        } else if (levelOf(line.text) === "error" || levelOf(line.text) === "critical") {
            results.logErrors.push(`${line.file}: ${line.text}`);
        }
    }
}

/// A failure for each unit clice failed or reported errors on that the
/// compiler compiles or cannot check; the others are the compiler's too.
async function judgeUnits(
    units: { unit: string; errors?: string[]; request?: string; repro: string }[],
    entries: readonly DatabaseEntry[],
    checkout: string,
    results: Results,
): Promise<void> {
    for (const unit of await confirm(entries, units)) {
        if (unit.outcome === "rejected") {
            results.compilerRejects.push({ unit: unit.unit, output: unit.output });
            continue;
        }
        const unchecked =
            unit.outcome === "unchecked" ? [`the compiler could not check it: ${unit.output}`] : [];
        results.failures.push({
            kind: unit.errors === undefined ? "index" : "compile",
            file: inside(checkout, unit.unit) ?? unit.unit,
            ...(unit.request === undefined ? {} : { request: unit.request }),
            detail: [
                ...(unit.errors ?? ["the unit failed to index; the session log says why"]),
                ...unchecked,
            ].join("\n"),
            repro: unit.repro,
        });
    }
}

function located(error: { file: string; line: number; column: number; message: string }): string {
    return `${error.file === "" ? "" : `${error.file}:${String(error.line)}:${String(error.column)}: `}${error.message}`;
}

async function indexStep(
    options: NightlyOptions,
    checkout: string,
    entries: readonly DatabaseEntry[],
    results: Results,
): Promise<void> {
    if (fs.existsSync(path.join(checkout, ".clice"))) {
        console.warn(
            `${checkout}/.clice exists: the index starts warm, its numbers are no cold start`,
        );
    }
    const run = await runIndex(checkout);
    const report = run.report;
    const again = rerun(options, "--steps index");
    if (run.hung !== undefined) {
        results.failures.push({
            kind: "hang",
            detail: `no unit finished for half an hour; in flight:\n${run.hung.join("\n")}`,
            repro: again,
        });
    } else if (run.overTime !== undefined) {
        results.failures.push({
            kind: "unfinished",
            detail: `clice index was stopped at its three-hour bound, at ${run.overTime} units`,
            repro: again,
        });
    } else if (report === undefined || (run.status !== 0 && run.status !== 1)) {
        results.failures.push({
            kind: "crash",
            detail: `clice index ended with ${run.signal ?? `exit ${String(run.status)}`}${run.memory === undefined ? "" : `, its processes at ${String(run.memory.total)} MB at most`}:\n${run.stderr}`,
            repro: again,
        });
    } else if (report.completed === undefined) {
        results.failures.push({
            kind: "unfinished",
            detail: `a clice server already serves ${checkout}, and clice index handed the run to it: stop that server`,
            repro: again,
        });
        return;
    } else if (!report.completed || report.unsaved) {
        results.failures.push({
            kind: "unfinished",
            detail: `clice index ${report.completed ? "could not save the index" : "ended before its run finished"} (exit ${String(run.status)}):\n${run.stderr}`,
            repro: again,
        });
    }
    if (report === undefined) {
        return;
    }
    results.index = {
        seconds: run.seconds,
        units: report.indexedTus,
        shards: report.shardCount,
        shardBytes: report.shardBytes,
        symbols: report.symbolCount,
        cacheBytes: run.cacheBytes,
        ...(run.memory === undefined ? {} : { memory: run.memory }),
        compiledWithErrors: report.compiledWithErrors.length,
        failed: report.failed.length,
    };
    await judgeUnits(
        [
            ...report.failed.map((unit) => ({ unit, repro: again })),
            ...report.compiledWithErrors.map(({ unit, errors }) => ({
                unit,
                errors: errors.slice(0, 5).map(located),
                repro: `cd ${checkout} && ${options.clice} inspect folding-range ${unit}`,
            })),
        ],
        entries,
        checkout,
        results,
    );
}

async function sweepStep(
    options: NightlyOptions,
    checkout: string,
    entries: readonly DatabaseEntry[],
    results: Results,
    jobEnd: number,
): Promise<void> {
    const { job } = options;
    const units = entries.flatMap((entry) => inside(checkout, entrySource(entry)) ?? []);
    const files =
        options.files ?? sampleFiles(units, job.project.sample, `${options.seed} ${job.name}`);
    const run = await sweep({
        root: checkout,
        files,
        deadline: Math.min(Date.now() + SWEEP_MS, jobEnd),
        ...(options.only === undefined ? {} : { only: options.only }),
    });
    results.sweep = {
        files: run.files.length,
        serverOnly: run.serverOnly,
        requests: run.requests,
        inspectSeconds: run.inspectSeconds,
        serverSeconds: run.serverSeconds,
        slow: run.slow,
    };
    if (run.unasked.length > 0 || run.cut.length > 0) {
        results.failures.push({
            kind: "hang",
            detail: [
                `the sweep ran out of its time with ${String(run.unasked.length)} of ${String(run.files.length)} files unasked and ${String(run.cut.length)} asked in part:`,
                ...run.unasked,
                ...run.cut.map((file) => `${file} (in part)`),
            ].join("\n"),
            repro: rerun(options, "--steps sweep"),
        });
    }
    for (const exit of run.serverExit) {
        results.failures.push({
            kind: exit.includes("did not exit within") ? "hang" : "crash",
            detail: `the server's exit: ${exit}`,
            repro: rerun(options, "--steps sweep"),
        });
    }
    results.accepted.push(...run.accepted);
    for (const failure of run.failures) {
        results.failures.push({
            kind: failure.kind,
            file: failure.file,
            request: failure.label,
            detail: failure.detail,
            repro: rerun(options, `--steps sweep --file ${failure.file} --only ${failure.request}`),
        });
    }
    // A file the index step judged already is reported once.
    const judged = new Set(
        results.failures.flatMap((failure) => (failure.kind === "compile" ? [failure.file] : [])),
    );
    for (const reject of results.compilerRejects) {
        judged.add(inside(checkout, reject.unit) ?? reject.unit);
    }
    await judgeUnits(
        run.serverErrors
            .filter(({ file }) => !judged.has(file))
            .map(({ file, errors }) => ({
                unit: path.join(checkout, file),
                errors,
                request: "errors",
                repro: rerun(options, `--steps sweep --file ${file} --only errors`),
            })),
        entries,
        checkout,
        results,
    );
}

export async function runNightly(options: NightlyOptions): Promise<Results> {
    const { job } = options;
    const results: Results = {
        job: job.name,
        platform: options.platform,
        about: job.project.about,
        commit: job.project.commit ?? cliceCommit(),
        clice: cliceCommit(),
        seed: options.seed,
        compilerRejects: [],
        logErrors: [],
        failures: [],
        accepted: [],
    };

    const started = Date.now();
    let checkout: string;
    let entries: DatabaseEntry[];
    try {
        checkout = await prepare(job.project, options.dir);
        entries = jobDatabase(job, checkout);
    } catch (error) {
        results.failures.push({ kind: "setup", detail: String(error), repro: rerun(options, "") });
        return results;
    }
    results.prepareSeconds = (Date.now() - started) / 1000;
    const earlier = new Set(logFiles(checkout));

    const steps = [
        ["index", indexStep],
        ["sweep", (...step: Parameters<typeof indexStep>) => sweepStep(...step, started + JOB_MS)],
    ] as const;
    for (const [step, run] of steps) {
        if (!options.steps.has(step)) {
            continue;
        }
        try {
            await run(options, checkout, entries, results);
        } catch (error) {
            results.failures.push({
                kind: "runner",
                detail: error instanceof Error ? (error.stack ?? error.message) : String(error),
                repro: rerun(options, `--steps ${step}`),
            });
        }
    }

    scanLogs(checkout, earlier, results, options);
    return results;
}
