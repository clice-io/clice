import { spawnSync } from "node:child_process";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function runLint(ws: Workspace, ...flags: string[]) {
    return spawnSync(
        cliceExecutable(),
        ["lint", ...flags, "--workspace", ws.root, "--workers", "2"],
        { encoding: "utf8", timeout: 120_000 },
    );
}

function findings(stdout: string): string[] {
    return stdout.split("\n").filter((line) => /: (warning|error|note): /.test(line));
}

function runIndexStats(ws: Workspace) {
    return spawnSync(cliceExecutable(), ["index", "--stats", "--workspace", ws.root], {
        encoding: "utf8",
        timeout: 120_000,
    });
}

function writeRules(ws: Workspace) {
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\n',
    );
}

test("header findings merge across translation units", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\nHeaderFilterRegex: ".*"\n');
    ws.write("common.h", "#pragma once\ninline double rate(int a, int b) { return a / b; }\n");
    ws.write("a.cpp", '#include "common.h"\ndouble run_a() { return rate(1, 2); }\n');
    ws.write("b.cpp", '#include "common.h"\ndouble run_b(int a, int b) { return a / b; }\n');
    ws.writeCDB(["a.cpp", "b.cpp"]);

    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(2);
    expect(lines[0]).toContain("b.cpp:2:");
    expect(lines[1]).toContain("common.h:2:");
    expect(run.stdout).toContain("Linted 2 translation units");
});

test("notes follow their finding", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-argument-comment"\nHeaderFilterRegex: ".*"\n');
    ws.write("common.h", "#pragma once\nvoid call(bool enabled);\n");
    ws.write("main.cpp", '#include "common.h"\nvoid run() { call(/*disabled=*/true); }\n');
    ws.writeCDB(["main.cpp"]);

    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(2);
    expect(lines[0]).toContain("main.cpp:2:");
    expect(lines[0]).toContain("bugprone-argument-comment");
    expect(lines[1]).toContain("common.h:2:");
    expect(lines[1]).toContain(": note: ");
});

test("nolint comments count in headers", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    ws.write(
        "common.h",
        "#pragma once\nint* first() { return 0; }  // NOLINT(modernize-use-nullptr)\nint* second() { return 0; }\n",
    );
    ws.write("main.cpp", '#include "common.h"\nint main() { return 0; }\n');
    ws.writeCDB(["main.cpp"]);

    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("common.h:3:");
});

test("lint rule keeps files out", ({ session }) => {
    const ws = session.tmpdir();
    writeRules(ws);
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    ws.write("vendor/lib.h", "#pragma once\ninline int* lib() { return 0; }\n");
    ws.write("vendor/lib.cpp", '#include "lib.h"\nint* lib2() { return 0; }\n');
    ws.write("main.cpp", '#include "vendor/lib.h"\nint* own() { return 0; }\n');
    ws.writeCDB(["main.cpp", "vendor/lib.cpp"]);

    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("main.cpp:2:");
    expect(run.stdout).toContain("Linted 1 translation unit ");
});

test("compiler errors in excluded files still report", ({ session }) => {
    const ws = session.tmpdir();
    writeRules(ws);
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    ws.write("vendor/broken.h", "#pragma once\nint broken( { return 0; }\n");
    ws.write("main.cpp", '#include "vendor/broken.h"\nint main() { return 0; }\n');
    ws.writeCDB(["main.cpp"]);

    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(
        findings(run.stdout).some((line) => line.includes("broken.h:2:") && line.includes("error")),
    ).toBe(true);
});

test("findings with different notes stay apart", ({ session }) => {
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
    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(
        lines.filter((line) => line.includes("common.h:2:") && line.includes("warning")),
    ).toHaveLength(2);
    expect(lines.some((line) => line.includes("a.cpp:2:") && line.includes(": note: "))).toBe(true);
    expect(lines.some((line) => line.includes("b.cpp:2:") && line.includes(": note: "))).toBe(true);
});

test("relative workspace path", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("main.cpp", "double ratio(int a, int b) { return a / b; }\n");
    ws.writeCDB(["main.cpp"]);

    const run = spawnSync(cliceExecutable(), ["lint", "--workspace", ".", "--workers", "2"], {
        cwd: ws.root,
        encoding: "utf8",
        timeout: 120_000,
    });
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("main.cpp:1:");
    expect(run.stdout).toContain("Linted 1 translation unit ");
});

test("index rule keeps excluded units out of the index", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\nindex = false\n',
    );
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    ws.write("vendor/lib.cpp", "int* lib() { return 0; }\n");
    ws.write("main.cpp", "int* own() { return 0; }\n");
    ws.writeCDB(["main.cpp", "vendor/lib.cpp"]);

    const run = runLint(ws, "--index");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(findings(run.stdout)).toHaveLength(1);
    expect(run.stdout).toContain("Linted 1 translation unit ");

    const stats = runIndexStats(ws);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});
