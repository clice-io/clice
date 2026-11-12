import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { elideCrash } from "../render/lsp.ts";
import {
    normalize,
    rootRelative,
    splitCommand,
    stdoutText,
    undeclaredDeath,
    type CommandRun,
} from "./run.ts";

describe("splitCommand", () => {
    test("quotes group words", () => {
        expect(splitCommand(`query symbol-search "a b" --workspace='x y'  --limit 3`)).toEqual([
            "query",
            "symbol-search",
            "a b",
            "--workspace=x y",
            "--limit",
            "3",
        ]);
        expect(splitCommand(`format ""`)).toEqual(["format", ""]);
    });

    test("globs and variables stay as written, shell syntax is refused", () => {
        expect(splitCommand("analyze modules --scope core/** --out $OUT")).toEqual([
            "analyze",
            "modules",
            "--scope",
            "core/**",
            "--out",
            "$OUT",
        ]);
        expect(() => splitCommand("index | head")).toThrow(/shell syntax/);
        expect(() => splitCommand("index # comment")).toThrow(/shell syntax/);
    });
});

describe("undeclaredDeath", () => {
    const ended = (status: number | null, signal: NodeJS.Signals | null): CommandRun => ({
        label: "$ clice index",
        pid: 4242,
        transcript: {},
        status,
        signal,
        stdout: "",
        stderr: "",
        json: undefined,
    });

    test("a signal the test did not send fails", () => {
        expect(undeclaredDeath(ended(null, "SIGSEGV"), {})).toBe(
            "$ clice index crashed (SIGSEGV), which the test did not cause",
        );
        expect(undeclaredDeath(ended(null, "SIGPIPE"), { stdout: "merged" })).toBeDefined();
    });

    test("the signals the test sent, or a reader that left, do not", () => {
        const interrupt = { units: 1, signals: ["SIGTERM", "SIGHUP"] as NodeJS.Signals[] };
        expect(undeclaredDeath(ended(null, "SIGHUP"), { interrupt })).toBeUndefined();
        expect(undeclaredDeath(ended(null, "SIGPIPE"), { stdout: "closed early" })).toBeUndefined();
        expect(undeclaredDeath(ended(null, "SIGPIPE"), { stdout: "fifo gone" })).toBeUndefined();
        expect(undeclaredDeath(ended(130, null), { interrupt })).toBeUndefined();
        expect(undeclaredDeath(ended(1, null), {})).toBeUndefined();
    });
});

describe("normalize", () => {
    test("host details are elided", () => {
        const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-norm-")));
        try {
            const text = [
                `${root}/src/a.cpp`,
                `${root}/.clice/logs/2026-10-11_02-58-20_88194/master.log`,
                `${root}/logs/2026-10-11_02-58-20_88194`,
                "indexed 9 units in 1.25s, 3ms per unit, 12.5 KB",
                "built at 2026-10-12 04:41:13  generation=1",
            ].join("\n");
            expect(normalize(text, root).split("\n")).toEqual([
                "${WS}/src/a.cpp",
                "${WS}/.clice/logs/${SESSION}/master.log",
                "${WS}/logs/${SESSION}",
                "indexed 9 units in #s, #ms per unit, # KB",
                "built at #time  generation=1",
            ]);
        } finally {
            fs.rmSync(root, { recursive: true });
        }
    });

    test("a JSON report's durations and sizes are elided", () => {
        const report = { seconds: 1.5, shardBytes: 1024, mergeMs: 3, symbolCount: 10 };
        expect(JSON.parse(stdoutText(JSON.stringify(report), os.tmpdir()))).toEqual({
            seconds: "#",
            shardBytes: "#",
            mergeMs: "#",
            symbolCount: 10,
        });
    });

    test("a worker's death reads the same everywhere", () => {
        expect(elideCrash("Worker SL-0 killed by signal 4 (SIGILL)")).toBe("Worker SL-0 ${CRASH}");
        expect(
            elideCrash(
                "Worker SL-0 terminated by exception 0xC000001D (STATUS_ILLEGAL_INSTRUCTION)",
            ),
        ).toBe("Worker SL-0 ${CRASH}");
        expect(elideCrash("Worker SL-0 exited with code 1 (1 crash in a row)")).toBe(
            "Worker SL-0 ${CRASH} (1 crash in a row)",
        );
        expect(elideCrash("the compiler exited with code 1")).toBe(
            "the compiler exited with code 1",
        );
    });
});

describe("rootRelative", () => {
    test("a path holding spaces is read one directory at a time", () => {
        const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-rel-")));
        try {
            fs.mkdirSync(path.join(root, "my", "sub"), { recursive: true });
            fs.mkdirSync(path.join(root, "my dir", "sub"), { recursive: true });
            fs.writeFileSync(path.join(root, "my dir", "sub", "a b.h"), "");
            const text = `${root}\\my dir\\sub\\a b.h:3:1: note, and ${root}\\gone\\x.h`;
            expect(rootRelative(text, root, "\\", root)).toBe(
                "${WS}/my dir/sub/a b.h:3:1: note, and ${WS}/gone\\x.h",
            );
            const json = `"${root}\\\\my dir\\\\sub\\\\a b.h"`;
            expect(rootRelative(json, root, "\\\\", root)).toBe('"${WS}/my dir/sub/a b.h"');
        } finally {
            fs.rmSync(root, { recursive: true });
        }
    });
});
