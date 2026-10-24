/// One perf job: prepare the configuration, then run the project's script
/// over clice, clangd and a baseline clice when one is given. A clice run
/// that fails (a crash, a hang, an error answer, an unclean exit) fails the
/// job unless the project knows it; a clangd run that fails is listed;
/// numbers never fail.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { jobDatabase } from "../database.ts";
import { prepare } from "../prepare.ts";
import type { Platform } from "../project.ts";
import {
    cliceCommit,
    projectCommit,
    rerunCommand,
    sortKnown,
    type Failure,
    type PerfJob,
} from "../script.ts";
import type { PerfResults, RunResult, ServerName } from "./results.ts";
import { Perf } from "./scenarios.ts";
import { pinnedClangd } from "./servers.ts";

export interface PerfOptions {
    job: PerfJob;
    platform: Platform;
    /// Where projects are checked out, and clangd downloaded.
    dir: string;
    /// The clice under test, a clangd other than the pinned one, a second
    /// clice to compare with.
    clice: string;
    clangd?: string;
    baseline?: string;
    /// Only these scenarios of the script.
    only?: ReadonlySet<string>;
    dropCaches: boolean;
    /// Where the results go as they come: a job cut short keeps its runs.
    out?: string;
}

function rerun(options: PerfOptions, extra: string): string {
    return rerunCommand(options.job, options.clice, options.dir, extra);
}

/// What a clice run's errors say went wrong: a crash first, which takes
/// the requests it was asked down with it, then a hang (whose server the
/// run killed), then an error answer.
export function failureOf(run: RunResult, options: PerfOptions): Failure {
    const error = run.error ?? "";
    const kind = /crashed|exited while|exited with (?!SIGKILL)/.test(error)
        ? "crash"
        : /timed out after|not idle after|announced no|exited with SIGKILL/.test(error)
          ? "hang"
          : "server";
    return {
        kind,
        file: run.file ?? run.label,
        request: run.scenario,
        detail: `${run.server}${run.variant === undefined ? "" : ` (${run.variant})`}, run ${String(run.run)}: ${error}`,
        repro: rerun(options, `--scenario ${run.scenario}`),
    };
}

/// No scenario begins after this long into the job, and no wait goes past
/// it: what runs finishes and the results upload within the runner's six
/// hours.
const JOB_MS = 320 * 60_000;

export async function runPerf(options: PerfOptions): Promise<PerfResults> {
    const { job } = options;
    const started = Date.now();
    const cpus = os.cpus();
    const results: PerfResults = {
        job: job.name,
        platform: options.platform,
        about: `${job.project.about}; ${job.config.about}`,
        commit: projectCommit(job.project),
        clice: cliceCommit(),
        env: {
            platform: process.platform,
            release: os.release(),
            arch: process.arch,
            cpus: cpus.length,
            cpuModel: cpus[0]?.model ?? "unknown",
            memoryMB: Math.round(os.totalmem() / 1024 / 1024),
            node: process.version,
            date: new Date().toISOString(),
        },
        binaries: {},
        clangdArgs: [...job.clangdArgs],
        runs: [],
        failures: [],
        known: [],
        unseen: [],
    };
    const save = () => {
        if (options.out !== undefined) {
            fs.mkdirSync(options.out, { recursive: true });
            fs.writeFileSync(
                path.join(options.out, "results.json"),
                `${JSON.stringify(results, null, 1)}\n`,
            );
        }
    };
    let perf: Perf;
    try {
        const checkout = await prepare(job.project, job.configuration, options.dir);
        const { entries, directory } = jobDatabase(job.config, undefined, checkout);
        const build = path.dirname(path.join(checkout, job.config.database));
        const binaries: Partial<Record<ServerName, string>> = {
            clice: options.clice,
            clangd: options.clangd ?? (await pinnedClangd(options.dir)),
            ...(options.baseline === undefined ? {} : { baseline: options.baseline }),
        };
        results.binaries = binaries;
        perf = new Perf(
            {
                workspace: checkout,
                database: directory,
                ...(build === checkout ? {} : { build }),
                binaries,
                clangdArgs: job.clangdArgs,
                dropCaches: options.dropCaches,
                deadline: started + JOB_MS,
            },
            entries,
            results.runs,
            { ...(options.only === undefined ? {} : { only: options.only }), save },
        );
    } catch (error) {
        results.failures.push({ kind: "setup", detail: String(error), repro: rerun(options, "") });
        sortKnown(options.job, options.platform, results);
        save();
        return results;
    }
    try {
        await job.script(perf);
    } catch (error) {
        results.failures.push({
            kind: "runner",
            detail: error instanceof Error ? (error.stack ?? error.message) : String(error),
            repro: rerun(options, ""),
        });
    }
    results.failures.push(
        ...results.runs
            .filter((run) => run.error !== undefined && run.server === "clice")
            .map((run) => failureOf(run, options)),
    );
    if (perf.unrun.length > 0) {
        results.failures.push({
            kind: "unfinished",
            detail: `the job ran out of its time before:\n${perf.unrun.join("\n")}`,
            repro: rerun(options, ""),
        });
    }
    sortKnown(options.job, options.platform, results);
    save();
    return results;
}
