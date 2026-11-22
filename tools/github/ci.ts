/// A commit's CI read while it runs: every job of every workflow run on the
/// commit, finished or not, and the log of each job that finished, without
/// waiting for the rest of its run (the pr skill).
///
/// Usage:
///   node tools/github/ci.ts status [PR] [--sha <sha> | --run <id>]
///   node tools/github/ci.ts logs <job id>... [--lines <n>]
///   node tools/github/ci.ts logs --failed [PR] [--sha <sha> | --run <id>] [--lines <n>]
///
/// With neither a PR, `--sha` nor `--run`, the commit is the head of the
/// current branch's PR. `status` prints the result first: whether every job
/// finished, how many failed. `logs` writes each finished job's full log to
/// temp/ci/<run>/<job>.log and prints its failing step's errors and last
/// lines (`--lines`, default 60).

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { REPO_ROOT } from "../repo.ts";
import { ghJson, ghOrUndefined } from "./gh.ts";

export interface Step {
    name: string;
    status: string;
    conclusion: string | null;
}

export interface Job {
    id: number;
    run_id: number;
    name: string;
    status: string;
    conclusion: string | null;
    html_url: string;
    steps?: Step[];
}

export interface Run {
    id: number;
    name: string;
    event: string;
    status: string;
    conclusion: string | null;
    run_attempt: number;
    html_url: string;
}

/// The conclusions of a job that failed on its own; a cancelled job is
/// counted apart, as a failure elsewhere or a newer push cancels it.
const FAILED = new Set(["failure", "timed_out", "startup_failure"]);

export interface RunSummary {
    run: Run;
    total: number;
    finished: number;
    passed: number;
    skipped: number;
    cancelled: number;
    failed: { job: Job; step: string | undefined }[];
    running: Job[];
    queued: Job[];
}

export function summarize(run: Run, jobs: readonly Job[]): RunSummary {
    const summary: RunSummary = {
        run,
        total: jobs.length,
        finished: 0,
        passed: 0,
        skipped: 0,
        cancelled: 0,
        failed: [],
        running: [],
        queued: [],
    };
    for (const job of jobs) {
        if (job.status !== "completed") {
            (job.status === "in_progress" ? summary.running : summary.queued).push(job);
            continue;
        }
        summary.finished += 1;
        const conclusion = job.conclusion ?? "";
        if (FAILED.has(conclusion)) {
            const step = job.steps?.find((each) => FAILED.has(each.conclusion ?? ""))?.name;
            summary.failed.push({ job, step });
        } else if (conclusion === "cancelled") {
            summary.cancelled += 1;
        } else if (conclusion === "success") {
            summary.passed += 1;
        } else {
            summary.skipped += 1;
        }
    }
    return summary;
}

/// A run is finished when GitHub says so; its jobs alone can all look done
/// while it still has jobs to start.
function finished(summary: RunSummary): boolean {
    return summary.run.status === "completed";
}

/// The lines `status` prints: the result over every run first, then each
/// run's counts, failed jobs and the jobs still going.
export function report(label: string, summaries: readonly RunSummary[]): string[] {
    if (summaries.length === 0) {
        return [`${label}: no CI run yet`];
    }
    const total = summaries.reduce((sum, each) => sum + each.total, 0);
    const failed = summaries.reduce((sum, each) => sum + each.failed.length, 0);
    const going = summaries.reduce(
        (sum, each) => sum + each.running.length + each.queued.length,
        0,
    );
    const lines: string[] = [];
    if (summaries.every(finished)) {
        lines.push(
            failed === 0
                ? `${label}: CI finished, passed (${total} jobs)`
                : `${label}: CI finished, ${failed} of ${total} jobs failed`,
        );
    } else {
        const waiting = summaries.filter((each) => !finished(each) && each.total === 0).length;
        lines.push(
            `${label}: CI NOT FINISHED — ${going} of ${total} jobs still running or queued` +
                (waiting > 0 ? `, ${waiting} run(s) with no jobs yet` : "") +
                (failed > 0
                    ? `; ${failed} failed already, their logs are ready (ci.ts logs --failed)`
                    : ""),
        );
    }
    for (const summary of summaries) {
        const { run } = summary;
        const state = run.status === "completed" ? (run.conclusion ?? "") : run.status;
        lines.push(
            `run ${run.id} ${run.name} (${run.event}, attempt ${run.run_attempt}): ${state}  ${run.html_url}`,
        );
        if (summary.total === 0) {
            lines.push("  no jobs yet");
            continue;
        }
        lines.push(
            `  finished ${summary.finished} of ${summary.total}: ${summary.passed} passed, ` +
                `${summary.failed.length} failed, ${summary.cancelled} cancelled, ` +
                `${summary.skipped} skipped · ${summary.running.length} running · ` +
                `${summary.queued.length} queued`,
        );
        for (const { job, step } of summary.failed) {
            lines.push(
                `  failed   ${job.id}  ${job.name}` +
                    (step === undefined ? "" : ` — step "${step}"`),
            );
        }
        for (const [word, jobs] of [
            ["running", summary.running],
            ["queued", summary.queued],
        ] as const) {
            if (jobs.length > 0) {
                const names = jobs.slice(0, 6).map((job) => job.name);
                const more = jobs.length > 6 ? `, and ${jobs.length - 6} more` : "";
                lines.push(`  ${word.padEnd(8)} ${names.join(", ")}${more}`);
            }
        }
    }
    return lines;
}

/// The part of a job's log that tells why it failed: its failing step's
/// name, the errors the step reported and its last lines. The log's
/// timestamps are dropped, and so are the post-job steps after it.
export function digest(
    log: string,
    lines: number,
): { step: string | undefined; errors: string[]; tail: string[] } {
    const text = log
        .replace(/^\uFEFF/, "")
        .split(/\r?\n/)
        .map((line) => line.replace(/^\d{4}-\d\d-\d\dT[\d:.]+Z ?/, ""));
    const post = text.findIndex((line) => line.startsWith("Post job cleanup."));
    const end = post === -1 ? text.length : post;
    let start = 0;
    for (let index = end - 1; index >= 0; index -= 1) {
        if (text[index]?.startsWith("##[group]Run ")) {
            start = index;
            break;
        }
    }
    const step = start === 0 ? undefined : text[start]?.slice("##[group]".length);
    const errors = [
        ...new Set(
            text
                .slice(start, end)
                .filter((line) => line.startsWith("##[error]"))
                .map((line) => line.slice("##[error]".length)),
        ),
    ];
    const tail = text.slice(Math.max(start, end - lines), end).filter((line) => line !== "");
    return { step, errors, tail };
}

async function jobsOf(run: number): Promise<Job[]> {
    const jobs: Job[] = [];
    // Fifty a page: GitHub times out on a page of a hundred jobs with steps.
    for (let page = 1; ; page += 1) {
        const answer = await ghJson<{ total_count: number; jobs: Job[] }>([
            "api",
            `repos/{owner}/{repo}/actions/runs/${run}/jobs?per_page=50&page=${page}`,
        ]);
        jobs.push(...answer.jobs);
        if (answer.jobs.length === 0 || jobs.length >= answer.total_count) {
            return jobs;
        }
    }
}

async function runsOf(sha: string): Promise<Run[]> {
    const answer = await ghJson<{ workflow_runs: Run[] }>([
        "api",
        `repos/{owner}/{repo}/actions/runs?head_sha=${sha}&per_page=100`,
    ]);
    return answer.workflow_runs;
}

/// What `status` and `logs --failed` read: a label and the runs.
export interface Target {
    label: string;
    runs: Run[];
}

/// Each run summarized with its jobs.
export async function summariesOf(runs: readonly Run[]): Promise<RunSummary[]> {
    const summaries: RunSummary[] = [];
    for (const run of runs) {
        summaries.push(summarize(run, await jobsOf(run.id)));
    }
    return summaries;
}

/// The lines of `status` for a commit, for the PR reader too.
export async function statusOf(label: string, sha: string): Promise<string[]> {
    return report(label, await summariesOf(await runsOf(sha)));
}

async function resolveTarget(
    pr: string | undefined,
    sha: string | undefined,
    run: string | undefined,
): Promise<Target> {
    if (run !== undefined) {
        const one = await ghJson<Run>(["api", `repos/{owner}/{repo}/actions/runs/${run}`]);
        return { label: `run ${one.id}`, runs: [one] };
    }
    if (sha !== undefined) {
        return { label: sha.slice(0, 9), runs: await runsOf(sha) };
    }
    const view = await ghJson<{ number: number; headRefOid: string }>([
        "pr",
        "view",
        ...(pr === undefined ? [] : [pr]),
        "--json",
        "number,headRefOid",
    ]);
    return {
        label: `#${view.number} @ ${view.headRefOid.slice(0, 9)}`,
        runs: await runsOf(view.headRefOid),
    };
}

/// One finished job's log written under temp/ci/ and its digest printed.
async function printLog(job: Job, lines: number): Promise<void> {
    const header = `job ${job.id} ${job.name}`;
    if (job.status !== "completed") {
        console.log(`${header}: ${job.status} — its log comes when the job finishes`);
        return;
    }
    const log = await ghOrUndefined(["api", `repos/{owner}/{repo}/actions/jobs/${job.id}/logs`]);
    if (log === undefined) {
        console.log(`${header}: ${job.conclusion ?? ""} — GitHub has no log for it (expired?)`);
        return;
    }
    const file = path.join(REPO_ROOT, "temp", "ci", String(job.run_id), `${job.id}.log`);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, log);
    const { step, errors, tail } = digest(log, lines);
    const failed = job.steps?.find((each) => FAILED.has(each.conclusion ?? ""))?.name ?? step;
    console.log(
        `${header}: ${job.conclusion ?? ""}${failed === undefined ? "" : ` — step "${failed}"`}`,
    );
    for (const error of errors.slice(0, 10)) {
        console.log(`  error: ${error}`);
    }
    console.log(`  last lines:`);
    for (const line of tail) {
        console.log(`    ${line}`);
    }
    console.log(`  full log: ${path.relative(process.cwd(), file)}`);
}

async function main(): Promise<void> {
    const { values, positionals } = parseArgs({
        options: {
            sha: { type: "string" },
            run: { type: "string" },
            failed: { type: "boolean", default: false },
            lines: { type: "string", default: "60" },
        },
        allowPositionals: true,
    });
    const [command, ...rest] = positionals;
    const lines = Number(values.lines);
    if (command === "status") {
        const target = await resolveTarget(rest[0], values.sha, values.run);
        console.log(report(target.label, await summariesOf(target.runs)).join("\n"));
    } else if (command === "logs" && values.failed) {
        const target = await resolveTarget(rest[0], values.sha, values.run);
        const failed = (await summariesOf(target.runs)).flatMap((each) =>
            each.failed.map(({ job }) => job),
        );
        if (failed.length === 0) {
            console.log(`${target.label}: no job failed (yet)`);
        }
        for (const job of failed) {
            await printLog(job, lines);
        }
    } else if (command === "logs" && rest.length > 0) {
        for (const id of rest) {
            await printLog(
                await ghJson<Job>(["api", `repos/{owner}/{repo}/actions/jobs/${id}`]),
                lines,
            );
        }
    } else {
        console.error(
            "usage: node tools/github/ci.ts status [PR] [--sha <sha> | --run <id>]\n" +
                "       node tools/github/ci.ts logs <job id>... | --failed [PR] [--sha <sha> | --run <id>] [--lines <n>]",
        );
        process.exit(2);
    }
}

if (import.meta.main) {
    await main();
}
