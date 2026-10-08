import * as fs from "node:fs";
import { runProcess } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function runLint(ws: Workspace, ...flags: string[]) {
    return runProcess(
        cliceExecutable(),
        ["lint", ...flags, "--workspace", ws.root, "--workers", "2"],
        { timeout: 120_000 },
    );
}

function findings(stdout: string): string[] {
    return stdout.split("\n").filter((line) => /: (warning|error|note): /.test(line));
}

function runIndexStats(ws: Workspace) {
    return runProcess(cliceExecutable(), ["index", "--stats", "--workspace", ws.root], {
        timeout: 120_000,
    });
}

function writeRules(ws: Workspace) {
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\n',
    );
}

test("header findings merge across translation units", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\nHeaderFilterRegex: ".*"\n');
    ws.write("common.h", "#pragma once\ninline double rate(int a, int b) { return a / b; }\n");
    ws.write("a.cpp", '#include "common.h"\ndouble run_a() { return rate(1, 2); }\n');
    ws.write("b.cpp", '#include "common.h"\ndouble run_b(int a, int b) { return a / b; }\n');
    ws.writeCDB(["a.cpp", "b.cpp"]);

    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(2);
    expect(lines[0]).toContain("b.cpp:2:");
    expect(lines[1]).toContain("common.h:2:");
    expect(run.stdout).toContain("Linted 2 translation units");
});

test("notes follow their finding", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-argument-comment"\nHeaderFilterRegex: ".*"\n');
    ws.write("common.h", "#pragma once\nvoid call(bool enabled);\n");
    ws.write("main.cpp", '#include "common.h"\nvoid run() { call(/*disabled=*/true); }\n');
    ws.writeCDB(["main.cpp"]);

    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(2);
    expect(lines[0]).toContain("main.cpp:2:");
    expect(lines[0]).toContain("bugprone-argument-comment");
    expect(lines[1]).toContain("common.h:2:");
    expect(lines[1]).toContain(": note: ");
});

test("nolint comments count in headers", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    ws.write(
        "common.h",
        "#pragma once\nint* first() { return 0; }  // NOLINT(modernize-use-nullptr)\nint* second() { return 0; }\n",
    );
    ws.write("main.cpp", '#include "common.h"\nint main() { return 0; }\n');
    ws.writeCDB(["main.cpp"]);

    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("common.h:3:");
});

test("compiler warnings report under their check", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    ws.write("main.cpp", "int main() { int unused; return 0; }\n");
    ws.writeCDB(["main.cpp"], { extraArgs: ["-Wunused-variable"] });

    const quiet = await runLint(ws);
    expect(quiet.status, `stderr: ${quiet.stderr}`).toBe(0);
    expect(findings(quiet.stdout)).toEqual([]);

    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr,clang-diagnostic-unused-variable"\n');
    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("[clang-diagnostic-unused-variable]");
});

test("lint rule keeps files out", async ({ session }) => {
    const ws = session.tmpdir();
    writeRules(ws);
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    ws.write("vendor/lib.h", "#pragma once\ninline int* lib() { return 0; }\n");
    ws.write("vendor/lib.cpp", '#include "lib.h"\nint* lib2() { return 0; }\n');
    ws.write("main.cpp", '#include "vendor/lib.h"\nint* own() { return 0; }\n');
    ws.writeCDB(["main.cpp", "vendor/lib.cpp"]);

    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("main.cpp:2:");
    expect(run.stdout).toContain("Linted 1 translation unit ");
});

test("compiler errors in excluded files still report", async ({ session }) => {
    const ws = session.tmpdir();
    writeRules(ws);
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    ws.write("vendor/broken.h", "#pragma once\nint broken( { return 0; }\n");
    ws.write("main.cpp", '#include "vendor/broken.h"\nint main() { return 0; }\n');
    ws.writeCDB(["main.cpp"]);

    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(
        findings(run.stdout).some((line) => line.includes("broken.h:2:") && line.includes("error")),
    ).toBe(true);
});

test("findings with different notes stay apart", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(
        ".clang-tidy",
        'Checks: "-*,readability-inconsistent-declaration-parameter-name"\nHeaderFilterRegex: ".*"\n',
    );
    ws.write("common.h", "#pragma once\nvoid function(int original);\n");
    ws.write("a.cpp", '#include "common.h"\nvoid function(int alpha) { (void)alpha; }\n');
    ws.write("b.cpp", '#include "common.h"\nvoid function(int beta) { (void)beta; }\n');
    ws.writeCDB(["a.cpp", "b.cpp"]);

    // The same warning on the header's line from both units, each with
    // notes naming its own definition: two findings, not one.
    const run = await runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(
        lines.filter((line) => line.includes("common.h:2:") && line.includes("warning")),
    ).toHaveLength(2);
    expect(lines.some((line) => line.includes("a.cpp:2:") && line.includes(": note: "))).toBe(true);
    expect(lines.some((line) => line.includes("b.cpp:2:") && line.includes(": note: "))).toBe(true);
});

test("relative workspace path", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("main.cpp", "double ratio(int a, int b) { return a / b; }\n");
    ws.writeCDB(["main.cpp"]);

    const run = await runProcess(
        cliceExecutable(),
        ["lint", "--workspace", ".", "--workers", "2"],
        {
            cwd: ws.root,
            timeout: 120_000,
        },
    );
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("main.cpp:1:");
    expect(run.stdout).toContain("Linted 1 translation unit ");
});

test("index rule keeps excluded units out of the index", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\nindex = false\n',
    );
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    ws.write("vendor/lib.cpp", "int* lib() { return 0; }\n");
    ws.write("main.cpp", "int* own() { return 0; }\n");
    ws.writeCDB(["main.cpp", "vendor/lib.cpp"]);

    const run = await runLint(ws, "--index");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(findings(run.stdout)).toHaveLength(1);
    expect(run.stdout).toContain("Linted 1 translation unit ");

    const stats = await runIndexStats(ws);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});

test.skipIf(process.platform === "win32")(
    "findings name files under the workspace spelling",
    async ({ session }) => {
        const real = session.tmpdir();
        const outer = session.tmpdir();
        fs.symlinkSync(real.root, outer.path("ws"));
        const ws = new Workspace(outer.path("ws"));
        ws.pinCacheDir();
        ws.write(
            ".clang-tidy",
            'Checks: "-*,bugprone-argument-comment"\nHeaderFilterRegex: ".*"\n',
        );
        ws.write("common.h", "#pragma once\nvoid call(bool enabled);\n");
        ws.write("main.cpp", '#include "common.h"\nvoid run() { call(/*disabled=*/true); }\n');
        ws.writeCDB(["main.cpp"]);

        const run = await runLint(ws);
        expect(run.status, `stderr: ${run.stderr}`).toBe(1);
        const lines = findings(run.stdout);
        expect(lines).toHaveLength(2);
        expect(lines[0]!.startsWith(`${ws.path("main.cpp")}:2:`), lines[0]).toBe(true);
        expect(lines[1]!.startsWith(`${ws.path("common.h")}:2:`), lines[1]).toBe(true);
    },
);

test.skipIf(process.platform === "win32")(
    "configuration found from the database's name",
    async ({ session }) => {
        const ws = session.tmpdir();
        ws.pinCacheDir();
        ws.write(".clang-tidy", 'Checks: "-*"\n');
        ws.write("vendor/.clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
        ws.write("vendor/real/main.cpp", "double rate(int a, int b) { return a / b; }\n");
        fs.symlinkSync(ws.path("vendor/real"), ws.path("src"));
        ws.writeCDB(["src/main.cpp"]);

        const run = await runLint(ws);
        expect(run.status, `stderr: ${run.stderr}`).toBe(0);
        expect(findings(run.stdout)).toEqual([]);
    },
);

test.skipIf(process.platform === "win32")(
    "header filter matches the include's spelling",
    async ({ session }) => {
        const ws = session.tmpdir();
        ws.pinCacheDir();
        ws.write(
            ".clang-tidy",
            'Checks: "-*,bugprone-integer-division"\nHeaderFilterRegex: "inc/"\n',
        );
        ws.write(
            "vendor/real/common.h",
            "#pragma once\ninline double rate(int a, int b) { return a / b; }\n",
        );
        fs.symlinkSync(ws.path("vendor/real"), ws.path("inc"));
        ws.write("a.cpp", '#include "common.h"\ndouble run_a() { return rate(1, 2); }\n');
        ws.writeCDB(["a.cpp"], { extraArgs: ["-Iinc"] });

        const run = await runLint(ws);
        expect(run.status, `stderr: ${run.stderr}`).toBe(1);
        const lines = findings(run.stdout);
        expect(lines).toHaveLength(1);
        expect(lines[0]).toContain("common.h:2:");
    },
);

test("crashing unit is not retried", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("poison.cpp", "int poison() { return 1; }\n");
    ws.write("healthy.cpp", "double healthy(int a, int b) { return a / b; }\n");
    ws.writeCDB(["poison.cpp", "healthy.cpp"]);
    const tag = `tuRun ${ws.displayPath("poison.cpp")}`;

    // A unit that crashed its worker would crash a retry too: the sweep
    // gives up on it at once and still checks the rest.
    const run = await runProcess(
        cliceExecutable(),
        ["lint", "--workspace", ws.root, "--workers", "2"],
        {
            timeout: 120_000,
            env: { ...process.env, CLICE_ANOMALY_NO_TRAP: "1", CLICE_TEST_CRASH_REQUEST: tag },
        },
    );
    expect(run.stderr.split(`] clice worker crashed in: clice/worker/${tag}`).length - 1).toBe(1);
    expect(run.stderr).toContain("Lint gave up on");
    expect(run.stderr).not.toContain("after a retry");
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("healthy.cpp:1:");
});
