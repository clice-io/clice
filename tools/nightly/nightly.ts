/// One job of the nightly run: prepare the project, index it from a cold
/// cache, confirm with the compiler every unit clice reported errors on,
/// ask the features on sampled files, and collect what failed with how to
/// reproduce it. Counts, sizes, memory and times are recorded, never judged.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { entrySource } from "../compat/database.ts";
import { REPO_ROOT } from "../repo.ts";
import { levelOf, readLogs } from "../testing/logs/logs.ts";
import { CRASHED_IN } from "../testing/server/serve.ts";
import { confirm } from "./confirm.ts";
import { jobDatabase } from "./database.ts";
import { runIndex } from "./index.ts";
import type { PeakMemory } from "./memory.ts";
import { prepare } from "./prepare.ts";
import type { Job } from "./projects.ts";
import { sampleFiles, sweep, type Request } from "./sweep.ts";

export type Step = "index" | "sweep";

export interface Failure {
    kind:
        | "setup"
        | "crash"
        | "hang"
        | "index"
        | "compile"
        | "disagree"
        | "inspect"
        | "server"
        | "anomaly";
    file?: string;
    request?: string;
    detail: string;
    /// The command that reproduces it, run from the repository root.
    repro: string;
}

export interface Results {
    job: string;
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
        memory: PeakMemory;
        compiledWithErrors: number;
        failed: number;
    };
    sweep?: {
        files: number;
        importers: number;
        requests: number;
        inspectSeconds: number;
        serverSeconds: number;
    };
    /// Units clice reported errors on that the compiler rejects too.
    compilerRejects: { unit: string; output: string }[];
    /// Error-level lines of clice's logs: what went wrong without failing.
    logErrors: string[];
    failures: Failure[];
}

export interface NightlyOptions {
    job: Job;
    /// Where projects are checked out.
    dir: string;
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
    const relative = path.relative(checkout, real);
    return relative.startsWith("..") || path.isAbsolute(relative)
        ? null
        : relative.split(path.sep).join("/");
}

/// Crashes and anomalies in the logs of every clice process the job ran
/// fail it; the other error-level lines are recorded.
function scanLogs(checkout: string, results: Results, options: NightlyOptions): void {
    for (const line of readLogs(checkout)) {
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

export async function runNightly(options: NightlyOptions): Promise<Results> {
    const { job } = options;
    const results: Results = {
        job: job.name,
        about: job.project.about,
        commit: job.project.commit ?? cliceCommit(),
        clice: cliceCommit(),
        seed: options.seed,
        compilerRejects: [],
        logErrors: [],
        failures: [],
    };

    let started = Date.now();
    let checkout: string;
    try {
        checkout = await prepare(job.project, options.dir);
    } catch (error) {
        results.failures.push({
            kind: "setup",
            detail: String(error),
            repro: rerun(options, ""),
        });
        return results;
    }
    results.prepareSeconds = (Date.now() - started) / 1000;
    const entries = jobDatabase(job, checkout);

    if (options.steps.has("index")) {
        if (fs.existsSync(path.join(checkout, ".clice"))) {
            console.warn(
                `${checkout}/.clice exists: the index starts warm, its numbers are no cold start`,
            );
        }
        const run = await runIndex(options.clice, checkout);
        const report = run.report;
        if (run.hung !== undefined) {
            results.failures.push({
                kind: "hang",
                detail: `no unit finished for half an hour; in flight:\n${run.hung.join("\n")}`,
                repro: rerun(options, "--steps index"),
            });
        } else if (report === undefined || (run.status !== 0 && run.status !== 1)) {
            results.failures.push({
                kind: "crash",
                detail: `clice index ended with ${run.signal ?? `exit ${String(run.status)}`}:\n${run.stderr}`,
                repro: rerun(options, "--steps index"),
            });
        }
        if (report !== undefined) {
            results.index = {
                seconds: run.seconds,
                units: report.indexedTus,
                shards: report.shardCount,
                shardBytes: report.shardBytes,
                symbols: report.symbolCount,
                cacheBytes: run.cacheBytes,
                memory: run.memory,
                compiledWithErrors: report.compiledWithErrors.length,
                failed: report.failed.length,
            };
            const errors = new Map(
                report.compiledWithErrors.map((unit) => [unit.unit, unit.errors]),
            );
            const verdicts = await confirm(entries, [...report.failed, ...errors.keys()]);
            for (const verdict of verdicts) {
                if (!verdict.accepted) {
                    results.compilerRejects.push({ unit: verdict.unit, output: verdict.output });
                    continue;
                }
                const unitErrors = errors.get(verdict.unit);
                results.failures.push({
                    kind: unitErrors === undefined ? "index" : "compile",
                    file: inside(checkout, verdict.unit) ?? verdict.unit,
                    detail:
                        unitErrors === undefined
                            ? "the unit failed to index; the session log says why"
                            : unitErrors
                                  .slice(0, 5)
                                  .map(
                                      (error) =>
                                          `${error.file === "" ? "" : `${error.file}:${String(error.line)}:${String(error.column)}: `}${error.message}`,
                                  )
                                  .join("\n"),
                    repro: `cd ${checkout} && ${options.clice} inspect folding-range ${verdict.unit}`,
                });
            }
        }
    }

    if (options.steps.has("sweep")) {
        const units = entries.flatMap((entry) => inside(checkout, entrySource(entry)) ?? []);
        const files =
            options.files ?? sampleFiles(units, job.project.sample, `${options.seed} ${job.name}`);
        started = Date.now();
        const run = await sweep({
            root: checkout,
            files,
            ...(options.only === undefined ? {} : { only: options.only }),
            concurrency: 3,
        });
        results.sweep = {
            files: run.files.length,
            importers: run.importers.length,
            requests: run.requests,
            inspectSeconds: run.inspectSeconds,
            serverSeconds: run.serverSeconds,
        };
        for (const failure of run.failures) {
            results.failures.push({
                kind: failure.kind,
                file: failure.file,
                request: failure.label,
                detail: failure.detail,
                repro: rerun(
                    options,
                    `--steps sweep --file ${failure.file} --only ${failure.request}`,
                ),
            });
        }
    }

    scanLogs(checkout, results, options);
    return results;
}
