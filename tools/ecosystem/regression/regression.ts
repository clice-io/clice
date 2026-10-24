/// One regression job: prepare the configuration, index it from a cold
/// cache, confirm with the compiler every unit clice reported errors on,
/// ask the features on sampled files, and collect what failed with how to
/// reproduce it, the failures the project knows apart. Counts, sizes,
/// memory and times are recorded, never judged.

import * as path from "node:path";
import { entrySource, type DatabaseEntry } from "../../compat/database.ts";
import { ANOMALY_PATTERN, logFiles } from "../../process_gate.ts";
import { readLogs, undeclaredErrors } from "../../testing/logs/logs.ts";
import { CRASHED_IN } from "../../testing/server/serve.ts";
import {
    includedHeaders,
    inPart,
    inside,
    jobDatabase,
    sampleFiles,
    sampleHeaders,
} from "../database.ts";
import type { PeakMemory } from "../memory.ts";
import { wipeCache } from "../perf/servers.ts";
import { compilerEnvironment, prepare } from "../prepare.ts";
import type { Platform } from "../project.ts";
import {
    cliceCommit,
    projectCommit,
    rerunCommand,
    sortKnown,
    type Failure,
    type Known,
    type RegressionJob,
} from "../script.ts";
import { confirm } from "./confirm.ts";
import { runIndex } from "./index.ts";
import { CONCURRENCY, sweep, type Request, type SweepRun } from "./sweep.ts";

export type Step = "index" | "sweep";

/// No file is swept after this long into the sweep, nor this long into the
/// job: the files in flight finish and the results upload within the
/// runner's six hours, after an index run that took its full bound.
const SWEEP_MS = 2 * 60 * 60_000;
const JOB_MS = 270 * 60_000;

/// What fails a regression job, by kind.
export const FAILURE_KINDS = [
    "setup",
    "crash",
    "hang",
    "unfinished",
    "index",
    "memory",
    "compile",
    "disagree",
    "inspect",
    "server",
    "anomaly",
    "runner",
] as const;

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
        memory?: PeakMemory;
        compiledWithErrors: number;
        failed: number;
    };
    sweep?: Pick<
        SweepRun,
        "serverOnly" | "requests" | "inspectSeconds" | "serverSeconds" | "slow" | "memory"
    > & { files: number };
    /// Units clice reported errors on that the compiler rejects too.
    compilerRejects: { unit: string; output: string }[];
    /// Error-level lines of clice's logs: what went wrong without failing.
    logErrors: string[];
    /// The failures the project does not know: what fails the job.
    failures: Failure[];
    /// The failures it knows, each with its issue, and the known issues the
    /// job did not see.
    known: (Failure & { issue: string })[];
    unseen: Known[];
    /// The places whose answers differ as the project accepts, each with the
    /// case pinning it.
    accepted: { file: string; label: string; case: string }[];
}

export interface RegressionOptions {
    job: RegressionJob;
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

/// What every step of a job works on.
interface Context {
    options: RegressionOptions;
    checkout: string;
    entries: readonly DatabaseEntry[];
    /// The environment the configuration's compiler runs in.
    env: NodeJS.ProcessEnv | undefined;
    results: Results;
}

/// The command running one step of this job again.
function rerun(options: RegressionOptions, extra: string): string {
    return rerunCommand(options.job, options.clice, options.dir, extra);
}

/// Crashes and anomalies in the logs of every clice process the job ran
/// fail it; the other error-level lines are recorded. The logs of earlier
/// runs (`earlier`) are not this job's.
function scanLogs({ options, checkout, results }: Context, earlier: ReadonlySet<string>): void {
    const lines = readLogs(
        checkout,
        logFiles(checkout).filter((file) => !earlier.has(file)),
    );
    for (const line of lines) {
        const crashed = CRASHED_IN.exec(line.text);
        if (crashed !== null) {
            const file = inside(checkout, crashed[2] ?? "") ?? crashed[2] ?? "";
            results.failures.push({
                kind: "crash",
                request: crashed[1] ?? "",
                file,
                detail: `${line.file}: ${line.text}`,
                repro: rerun(
                    options,
                    line.file === "index.log" ? "--steps index" : `--steps sweep --file ${file}`,
                ),
            });
        } else if (ANOMALY_PATTERN.test(line.text)) {
            results.failures.push({
                kind: "anomaly",
                detail: `${line.file}: ${line.text}`,
                repro: rerun(options, ""),
            });
        }
    }
    results.logErrors.push(
        ...undeclaredErrors(lines, [CRASHED_IN, ANOMALY_PATTERN]).map(
            (line) => `${line.file}: ${line.text}`,
        ),
    );
}

/// A failure for each unit clice failed or reported errors on that the
/// compiler compiles or cannot check; the others are the compiler's too.
async function judgeUnits(
    units: {
        unit: string;
        host?: string | undefined;
        errors?: string[];
        request?: string;
        repro: string;
    }[],
    { checkout, entries, env, results }: Context,
): Promise<void> {
    for (const unit of await confirm(entries, units, env)) {
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

async function indexStep(context: Context): Promise<void> {
    const { options, checkout, results } = context;
    wipeCache(checkout, checkout, "clice");
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
    } else if (!report.completed || report.unsaved || report.damaged) {
        const why = !report.completed
            ? "ended before its run finished"
            : report.unsaved
              ? "could not save the index"
              : "found the index database damaged and discarded it";
        results.failures.push({
            kind: "unfinished",
            detail: `clice index ${why} (exit ${String(run.status)}):\n${run.stderr}`,
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
    for (const { unit, memory } of report.tooBig ?? []) {
        results.failures.push({
            kind: "memory",
            file: inside(checkout, unit) ?? unit,
            detail: `clice index gave it up: its worker reached ${String(Math.round(memory / 1024 ** 2))} MB alone`,
            repro: again,
        });
    }
    await judgeUnits(
        [
            ...report.failed.map((unit) => ({ unit, repro: again })),
            ...report.compiledWithErrors.map(({ unit, errors }) => ({
                unit,
                errors: errors.slice(0, 5).map(located),
                repro: `cd ${checkout} && ${options.clice} inspect folding-range ${unit}`,
            })),
        ],
        context,
    );
}

async function sweepStep(context: Context, jobEnd: number): Promise<void> {
    const { options, checkout, entries, results } = context;
    const { job } = options;
    const units = entries.flatMap((entry) => inside(checkout, entrySource(entry)) ?? []);
    const seed = `${options.seed} ${job.name}`;
    // A machine asking fewer files at once samples fewer: a sweep takes the
    // same time everywhere.
    const sample = Math.ceil((job.sample * CONCURRENCY) / 3);
    const hosts = await includedHeaders(checkout, options.clice, true);
    // A unit that another unit includes too (CPython includes .c files) is
    // asked of both clients and checked as itself.
    for (const unit of units) {
        hosts.delete(unit);
    }
    const files = options.files ?? [
        ...sampleFiles(units, sample, seed),
        ...sampleHeaders(
            [...hosts.keys()].filter((file) => inPart(job.config, job.part, checkout, file)),
            Math.ceil(sample / 4),
            seed,
        ),
    ];
    const run = await sweep({
        root: checkout,
        files,
        headers: new Set(files.filter((file) => hosts.has(file))),
        deadline: Math.min(Date.now() + SWEEP_MS, jobEnd),
        ...(options.only === undefined ? {} : { only: options.only }),
    });
    const { serverOnly, requests, inspectSeconds, serverSeconds, slow, memory } = run;
    results.sweep = {
        files: run.files.length,
        serverOnly,
        requests,
        inspectSeconds,
        serverSeconds,
        slow,
        ...(memory === undefined ? {} : { memory }),
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
    if (run.unsettled !== undefined) {
        results.failures.push({
            kind: "hang",
            detail: run.unsettled,
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
            request: failure.request,
            ...(failure.at === undefined ? {} : { at: failure.at }),
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
                host: hosts.get(file),
                errors,
                request: "errors",
                repro: rerun(options, `--steps sweep --file ${file} --only errors`),
            })),
        context,
    );
}

export async function runRegression(options: RegressionOptions): Promise<Results> {
    const { job } = options;
    const results: Results = {
        job: job.name,
        platform: options.platform,
        about: `${job.project.about}; ${job.config.about}`,
        commit: projectCommit(job.project),
        clice: cliceCommit(),
        seed: options.seed,
        compilerRejects: [],
        logErrors: [],
        failures: [],
        known: [],
        unseen: [],
        accepted: [],
    };

    const started = Date.now();
    let context: Context;
    try {
        const checkout = await prepare(job.project, job.configuration, options.dir);
        const { entries } = jobDatabase(job.config, job.part, checkout);
        const env = await compilerEnvironment(job.config);
        context = { options, checkout, entries, env, results };
    } catch (error) {
        results.failures.push({ kind: "setup", detail: String(error), repro: rerun(options, "") });
        sortKnown(options.job, options.platform, results);
        return results;
    }
    results.prepareSeconds = (Date.now() - started) / 1000;
    const earlier = new Set(logFiles(context.checkout));

    const steps = [
        ["index", indexStep],
        ["sweep", (step: Context) => sweepStep(step, started + JOB_MS)],
    ] as const;
    for (const [step, run] of steps) {
        if (!options.steps.has(step)) {
            continue;
        }
        try {
            await run(context);
        } catch (error) {
            results.failures.push({
                kind: "runner",
                detail: error instanceof Error ? (error.stack ?? error.message) : String(error),
                repro: rerun(options, `--steps ${step}`),
            });
        }
    }

    scanLogs(context, earlier);
    sortKnown(options.job, options.platform, results);
    return results;
}
