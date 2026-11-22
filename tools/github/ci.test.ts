import { describe, expect, test } from "vitest";
import { digest, report, summarize, type Job, type Run } from "./ci.ts";

const run = (status: string, conclusion: string | null = null): Run => ({
    id: 7,
    name: "main",
    event: "pull_request",
    status,
    conclusion,
    run_attempt: 1,
    html_url: "https://github.com/o/r/actions/runs/7",
});

let next = 100;
const job = (
    name: string,
    status: string,
    conclusion: string | null = null,
    failedStep?: string,
): Job => {
    next += 1;
    return {
        id: next,
        run_id: 7,
        name,
        status,
        conclusion,
        html_url: "",
        steps: [
            { name: "Set up job", status: "completed", conclusion: "success" },
            ...(failedStep === undefined
                ? []
                : [{ name: failedStep, status: "completed", conclusion: "failure" }]),
        ],
    };
};

describe("summarize and report", () => {
    test("a run still going says so first, with the failures it already has", () => {
        const summary = summarize(run("in_progress"), [
            job("lint", "completed", "success"),
            job("test / linux", "completed", "failure", "Run cases"),
            job("test / windows", "in_progress"),
            job("test / macos", "queued"),
            job("release", "completed", "skipped"),
        ]);
        expect(summary.finished).toBe(3);
        const lines = report("#1", [summary]);
        expect(lines[0]).toBe(
            "#1: CI NOT FINISHED — 2 of 5 jobs still running or queued; 1 failed already, their logs are ready (ci.ts logs --failed)",
        );
        expect(lines).toContain(
            `  failed   ${String(summary.failed[0]?.job.id)}  test / linux — step "Run cases"`,
        );
        expect(lines).toContain("  running  test / windows");
        expect(lines).toContain("  queued   test / macos");
    });

    test("a finished run is passed or names its failures", () => {
        const passed = summarize(run("completed", "success"), [
            job("lint", "completed", "success"),
            job("release", "completed", "skipped"),
        ]);
        expect(report("#1", [passed])[0]).toBe("#1: CI finished, passed (2 jobs)");
        const failed = summarize(run("completed", "failure"), [
            job("lint", "completed", "timed_out"),
            job("test", "completed", "cancelled"),
        ]);
        expect(failed.cancelled).toBe(1);
        expect(report("#1", [failed])[0]).toBe("#1: CI finished, 1 of 2 jobs failed");
    });

    test("jobs all done in a run GitHub has not finished is no result", () => {
        const summary = summarize(run("in_progress"), [job("lint", "completed", "success")]);
        expect(report("#1", [summary])[0]).toMatch(/CI NOT FINISHED/);
    });

    test("no run yet", () => {
        expect(report("#1", [])).toEqual(["#1: no CI run yet"]);
    });
});

describe("digest", () => {
    const log = [
        "﻿2026-11-21T13:51:47.3195166Z Current runner version: '2.338.2'",
        "2026-11-21T13:51:48.0756216Z ##[group]Run actions/checkout@v7",
        "2026-11-21T13:51:48.0766571Z ##[endgroup]",
        "2026-11-21T13:52:00.0000000Z ##[group]Run pixi run compat-test",
        "2026-11-21T13:52:00.1000000Z ##[endgroup]",
        "2026-11-21T13:58:14.6645989Z ##[error]AssertionError: the build failed",
        "2026-11-21T13:58:14.6672939Z detail line",
        "2026-11-21T13:58:14.6672939Z ##[error]AssertionError: the build failed",
        "2026-11-21T13:58:14.7189078Z ##[error]Process completed with exit code 1.",
        "2026-11-21T13:58:14.7397373Z Post job cleanup.",
        "2026-11-21T13:58:14.7397373Z cleanup noise",
    ].join("\r\n");

    test("the failing step, its errors once each, its last lines", () => {
        expect(digest(log, 3)).toEqual({
            step: "Run pixi run compat-test",
            errors: ["AssertionError: the build failed", "Process completed with exit code 1."],
            tail: [
                "detail line",
                "##[error]AssertionError: the build failed",
                "##[error]Process completed with exit code 1.",
            ],
        });
    });
});
