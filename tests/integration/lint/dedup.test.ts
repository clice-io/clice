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

/// Three TUs over two headers: a template instantiated differently per TU,
/// a macro-conditional declaration, an out-of-line definition, a NOLINT,
/// and a finding whose note points into a header.
function writeProject(ws: Workspace) {
    ws.write(
        ".clang-tidy",
        'Checks: "-*,bugprone-integer-division,modernize-use-nullptr,bugprone-argument-comment"\n' +
            'HeaderFilterRegex: ".*"\n',
    );
    ws.write(
        "common.h",
        [
            "#pragma once",
            "inline int* null_ptr() { return 0; }",
            "int* suppressed() { return 0; }  // NOLINT(modernize-use-nullptr)",
            "#ifdef WIDE",
            "inline double wide_ratio(int a, int b) { return a / b; }",
            "#endif",
            "void call(bool enabled);",
            "template <class T> struct Box {",
            "    T value;",
            "    T get() const { return value; }",
            "    double ratio(T d) const { return value / d; }",
            "};",
            "",
        ].join("\n"),
    );
    ws.write(
        "other.h",
        [
            "#pragma once",
            '#include "common.h"',
            "inline double rate(int a, int b) { return a / b; }",
            "",
        ].join("\n"),
    );
    ws.write(
        "a.cpp",
        [
            '#include "common.h"',
            '#include "other.h"',
            "void call(bool enabled) { (void)enabled; }",
            "double run_a() { return Box<int>{3}.get() + rate(1, 2); }",
            "",
        ].join("\n"),
    );
    ws.write(
        "b.cpp",
        [
            '#include "other.h"',
            '#include "common.h"',
            "double run_b() {",
            "    call(/*disabled=*/true);",
            "    return Box<double>{3.0}.ratio(2.0) + Box<int>{4}.ratio(3);",
            "}",
            "",
        ].join("\n"),
    );
    ws.write(
        "c.cpp",
        ['#include "common.h"', "double run_c() { return wide_ratio(1, 2); }", ""].join("\n"),
    );
    ws.writeEntries(
        [
            ["a.cpp", []],
            ["b.cpp", []],
            ["c.cpp", ["-DWIDE"]],
        ],
        { std: "c++20" },
    );
}

test("dedup reports the whole runs' findings", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    writeProject(ws);

    const dedup = runLint(ws);
    expect(dedup.status, `stderr: ${dedup.stderr}`).toBe(1);
    const whole = runLint(ws, "--no-dedup");
    expect(whole.status, `stderr: ${whole.stderr}`).toBe(1);
    expect(findings(dedup.stdout)).toEqual(findings(whole.stdout));

    const lines = findings(dedup.stdout);
    // A header finding once, however many TUs include the header.
    expect(lines.filter((line) => line.includes("common.h:2:"))).toHaveLength(1);
    // The instantiation only b.cpp materializes, checked with the pattern
    // already claimed by a.cpp.
    expect(
        lines.some((line) => line.includes("common.h:11:") && line.includes("integer-division")),
    ).toBe(true);
    // A macro variant only c.cpp compiles.
    expect(lines.some((line) => line.includes("common.h:5:"))).toBe(true);
    expect(lines.some((line) => line.includes("common.h:3:"))).toBe(false);
    expect(lines.some((line) => line.includes("common.h:7:") && line.includes(": note: "))).toBe(
        true,
    );
    expect(dedup.stdout).toContain("Linted 3 translation units");
});

test("verify compares against whole runs", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    writeProject(ws);

    const run = runLint(ws, "--verify");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("Verification passed");
});

test("lint rule keeps files out", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\n',
    );
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

test("header filters keep configurations apart", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("common.h", "#pragma once\ninline int* shared() { return 0; }\n");
    // The strict configuration never reports the header, so its TU must
    // not claim the header's declarations away from the loose one.
    ws.write("strict/.clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    ws.write("strict/a.cpp", '#include "../common.h"\nint* a() { return shared(); }\n');
    ws.write("loose/.clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    ws.write("loose/b.cpp", '#include "../common.h"\nint* b() { return shared(); }\n');
    ws.writeCDB(["strict/a.cpp", "loose/b.cpp"]);

    const run = runLint(ws, "--workers", "1");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(findings(run.stdout).filter((line) => line.includes("common.h:2:"))).toHaveLength(1);
    expect(run.stdout).toContain("Linted 2 translation units");
});

test("a parse with errors claims nothing", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    ws.write("common.h", "#pragma once\ninline int* shared() { return 0; }\n");
    // The broken TU sees the header first; its fatal error must not mark
    // the header checked for the healthy TU that follows.
    ws.write("broken.cpp", '#include "common.h"\n#include "missing.h"\n');
    ws.write("good.cpp", '#include "common.h"\nint* g() { return shared(); }\n');
    ws.writeCDB(["broken.cpp", "good.cpp"]);

    const run = runLint(ws, "--workers", "1");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines.filter((line) => line.includes("common.h:2:"))).toHaveLength(1);
    expect(lines.some((line) => line.includes("broken.cpp:2:") && line.includes("error"))).toBe(
        true,
    );
});

test("checks anchored on the translation unit run whole", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,modernize-deprecated-headers"\n');
    ws.write("common.h", "#pragma once\ninline int one() { return 1; }\n");
    ws.write("other.cpp", '#include "common.h"\nint two() { return one(); }\n');
    // Nothing of its own to check and the header already claimed: the
    // check that matches the translation unit itself must still report.
    ws.write("stub.cpp", '#include <stdlib.h>\n#include "common.h"\n');
    ws.writeCDB(["other.cpp", "stub.cpp"]);

    const run = runLint(ws, "--workers", "1");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(findings(run.stdout).filter((line) => line.includes("stub.cpp:1:"))).toHaveLength(1);
});

test("a body continued in another file reports there", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\n',
    );
    ws.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\nHeaderFilterRegex: ".*"\n');
    // The wrapper's own declaration is nobody's to check; the one whose
    // body continues in members.h is.
    ws.write(
        "vendor/wrapper.h",
        '#pragma once\nint* vendor_only() { return 0; }\nstruct Wrapped {\n#include "../members.h"\n};\n',
    );
    ws.write("members.h", "int* member() { return 0; }\n");
    ws.write("main.cpp", '#include "vendor/wrapper.h"\nint main() { return 0; }\n');
    ws.writeCDB(["main.cpp"]);

    const run = runLint(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    const lines = findings(run.stdout);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain("members.h:1:");
});

test("compiler errors in excluded files still report", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nlint = false\n',
    );
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

test("verify needs the deduplicated run", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("main.cpp", "int main() { return 0; }\n");
    ws.writeCDB(["main.cpp"]);

    const run = runLint(ws, "--no-dedup", "--verify");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("drop --no-dedup");
});
