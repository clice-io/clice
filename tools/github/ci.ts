/// A commit's CI read while it runs: every job of every workflow run on the
/// commit, finished or not, and the log of each job that finished, without
/// waiting for the rest of its run (the pull-request skill).
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
import { ghJson, ghTry } from "./gh.ts";
import { atMost, count, issueNumber } from "./options.ts";

export interface Step {
    name: string;
    status: string;
    conclusion: string | null;
    started_at?: string | null;
}

export interface Job {
    id: number;
    run_id: number;
    name: string;
    status: string;
    conclusion: string | null;
    html_url: string;
    started_at?: string | null;
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

/// The conclusions of a finished run that passed.
const PASSED = new Set(["success", "skipped", "neutral"]);

/// A finished run that did not pass although none of its jobs failed: one
/// that never started its jobs (`startup_failure`, a workflow file GitHub
/// rejected), one waiting for approval (`action_required`), one cancelled.
function endedWithoutFailedJob(summary: RunSummary): boolean {
    return (
        finished(summary) &&
        summary.failed.length === 0 &&
        !PASSED.has(summary.run.conclusion ?? "")
    );
}

/// A running job's line: the step it is at and since when, which tells a slow
/// job from a stuck one while its log does not exist yet.
function runningLine(job: Job, now: number): string {
    const step = job.steps?.find((each) => each.status === "in_progress");
    const since = step?.started_at ?? job.started_at;
    const minutes = since ? Math.floor((now - Date.parse(since)) / 60_000) : undefined;
    return (
        `  running  ${String(job.id)}  ${job.name}` +
        (step === undefined ? "" : ` — step "${step.name}"`) +
        (minutes === undefined
            ? ""
            : minutes < 1
              ? " for under a minute"
              : ` for ${String(minutes)} min`)
    );
}

/// The lines `status` prints: the result over every run first, then each
/// run's counts, failed jobs and the jobs still going.
export function report(
    label: string,
    summaries: readonly RunSummary[],
    now: number = Date.now(),
): string[] {
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
    const ended = summaries.filter(endedWithoutFailedJob);
    if (summaries.every(finished)) {
        const failures = [
            ...(failed > 0 ? [`${failed} of ${total} jobs failed`] : []),
            ...ended.map((each) => `run ${each.run.id} ended ${each.run.conclusion ?? ""}`),
        ];
        lines.push(
            failures.length === 0
                ? `${label}: CI finished, passed (${total} jobs)`
                : `${label}: CI finished, ${failures.join("; ")}`,
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
            lines.push(finished(summary) ? "  no jobs" : "  no jobs yet");
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
        lines.push(...summary.running.map((job) => runningLine(job, now)));
        const { queued } = summary;
        if (queued.length > 0) {
            const names = queued.slice(0, 6).map((job) => job.name);
            const more = queued.length > 6 ? `, and ${String(queued.length - 6)} more` : "";
            lines.push(`  queued   ${names.join(", ")}${more}`);
        }
    }
    return lines;
}

/// The part of a job's log that tells why it failed: the step that reported
/// the last error — later steps, an upload of the failure's logs, are not
/// it — that step's errors and its last lines. The log's timestamps are
/// dropped, and so are the post-job steps.
export function digest(
    log: string,
    lines: number,
): { step: string | undefined; errors: string[]; tail: string[] } {
    const text = log
        .replace(/^\uFEFF/, "")
        .split(/\r?\n/)
        .map((line) => line.replace(/^\d{4}-\d\d-\d\dT[\d:.]+Z ?/, ""));
    const post = text.findIndex((line) => line.startsWith("Post job cleanup."));
    const jobEnd = post === -1 ? text.length : post;
    const isStep = (line: string | undefined): boolean =>
        line?.startsWith("##[group]Run ") ?? false;
    const lastError = text.slice(0, jobEnd).findLastIndex((line) => line.startsWith("##[error]"));
    const anchor = lastError === -1 ? jobEnd - 1 : lastError;
    let found = -1;
    for (let index = anchor; index >= 0; index -= 1) {
        if (isStep(text[index])) {
            found = index;
            break;
        }
    }
    const start = Math.max(found, 0);
    const next = text.slice(anchor + 1, jobEnd).findIndex(isStep);
    const end = next === -1 ? jobEnd : anchor + 1 + next;
    const step = found === -1 ? undefined : text[found]?.slice("##[group]".length);
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
        ...(pr === undefined ? [] : [String(issueNumber(pr, "the PR"))]),
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
    const answer = await ghTry(["api", `repos/{owner}/{repo}/actions/jobs/${job.id}/logs`]);
    if ("stderr" in answer) {
        console.log(`${header}: ${job.conclusion ?? ""} — no log: ${answer.stderr.trim()}`);
        return;
    }
    const log = answer.stdout;
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
    const lines = count(values.lines, "--lines");
    const help =
        "usage: node tools/github/ci.ts status [PR] [--sha <sha> | --run <id>]\n" +
        "       node tools/github/ci.ts logs <job id>... | --failed [PR] [--sha <sha> | --run <id>] [--lines <n>]";
    if (command === "status") {
        const target = await resolveTarget(atMost(rest, 1, help)[0], values.sha, values.run);
        console.log(report(target.label, await summariesOf(target.runs)).join("\n"));
    } else if (command === "logs" && values.failed) {
        const target = await resolveTarget(atMost(rest, 1, help)[0], values.sha, values.run);
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
            const job = issueNumber(id, "a job id");
            await printLog(
                await ghJson<Job>(["api", `repos/{owner}/{repo}/actions/jobs/${job}`]),
                lines,
            );
        }
    } else {
        console.error(help);
        process.exit(2);
    }
}

if (import.meta.main) {
    await main();
}
