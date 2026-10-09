import * as fs from "node:fs";
import { runProcess } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function runFormat(ws: Workspace, ...args: string[]) {
    return runProcess(cliceExecutable(), ["format", ...args, "--workspace", ws.root], {
        timeout: 120_000,
    });
}

const UNFORMATTED = "int   add(int a,int b){return a+b;}\n";
const FORMATTED = "int add(int a, int b) { return a + b; }\n";

/// A build of two units over a header, a vendored directory the rules keep
/// out, and a generated header the build includes from a system directory.
function writeProject(ws: Workspace) {
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\npatterns = ["vendor/**"]\nformat = false\n',
    );
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write("lib.h", "#pragma once\n" + UNFORMATTED);
    ws.write("vendor/vendored.h", "#pragma once\n" + UNFORMATTED);
    ws.write("sys/system.h", "#pragma once\n" + UNFORMATTED);
    ws.write(
        "a.cpp",
        '#include "lib.h"\n#include "vendor/vendored.h"\n#include <system.h>\n' + UNFORMATTED,
    );
    ws.write("b.cpp", '#include "lib.h"\nint   main(){return 0;}\n');
    ws.writeCDB(["a.cpp", "b.cpp"], { extraArgs: ["-isystem", ws.path("sys")] });
}

test("formats the build's own files in place", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = await runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Formatted 3 files");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + FORMATTED);
    expect(ws.read("a.cpp")).toContain(FORMATTED);
    expect(ws.read("b.cpp")).toBe('#include "lib.h"\nint main() { return 0; }\n');
    // Neither the excluded directory nor a system header is touched.
    expect(ws.read("vendor/vendored.h")).toBe("#pragma once\n" + UNFORMATTED);
    expect(ws.read("sys/system.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("check reports the files needing formatting", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const dirty = await runFormat(ws, "--check");
    expect(dirty.status, `stderr: ${dirty.stderr}`).toBe(1);
    expect(dirty.stdout).toContain("3 need formatting");
    expect(dirty.stderr).toContain("lib.h:2:");
    expect(dirty.stderr).toContain("clang-format-violations");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);

    expect((await runFormat(ws)).status).toBe(0);
    const clean = await runFormat(ws, "--check");
    expect(clean.status, `stderr: ${clean.stderr}`).toBe(0);
    expect(clean.stdout).toContain("all formatted");
});

test("paths narrow the set or add a file", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("loose.cpp", UNFORMATTED);

    // A file the build does not know is formatted when named; the others
    // stay as they are.
    const one = await runFormat(ws, "loose.cpp");
    expect(one.status, `stderr: ${one.stderr}`).toBe(0);
    expect(one.stdout).toContain("Formatted 1 file ");
    expect(ws.read("loose.cpp")).toBe(FORMATTED);
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);

    // A directory narrows the build's own files to those under it; the
    // rules still apply inside it.
    const dir = await runFormat(ws, "vendor");
    expect(dir.status, `stderr: ${dir.stderr}`).toBe(0);
    expect(dir.stdout).toContain("No files to format");
    expect(ws.read("vendor/vendored.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("a missing clang-format fails the run", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = await runFormat(ws, "--clang-format", ws.path("no-such-clang-format"));
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("clang-format not found");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("a database above the workspace is not a build tree", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write(
        "src/clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\ncompile_commands = [".."]\n',
    );
    ws.write("src/a.cpp", UNFORMATTED);
    ws.writeCDB(["src/a.cpp"]);

    const run = await runProcess(
        cliceExecutable(),
        ["format", "--check", "--workspace", ws.path("src")],
        { timeout: 120_000 },
    );
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("Checked 1 file ");
});

test("a missing file argument fails the run", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = await runFormat(ws, "missing.cpp");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("missing.cpp: no such file");
});

test("a non-source file argument fails the run", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("README.md", "# notes\n");

    const run = await runFormat(ws, "README.md");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("README.md: not a C-family source file");
    expect(ws.read("README.md")).toBe("# notes\n");
});

test("an invalid log level is a usage error", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = await runFormat(ws, "--log-level", "loud");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("invalid enum value: loud");
});

test("a workspace that does not exist fails the run", async ({ session }) => {
    const ws = session.tmpdir();
    const run = await runProcess(
        cliceExecutable(),
        ["format", "--check", "--workspace", ws.path("missing")],
        { timeout: 120_000 },
    );
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("not a directory");
});

test("a broken configuration fails the run", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("clice.toml", "[[rules\npatterns = [\n");

    const run = await runFormat(ws);
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("clice.toml");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("a broken compilation database fails the run", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("compile_commands.json", "{");

    const run = await runFormat(ws, "--check");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("compilation database could not be loaded");
});

test("only C-family units are formatted", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write("a.cpp", UNFORMATTED);
    ws.write("boot.s", "    mov r0, r1\n");
    ws.writeEntries(
        [
            ["a.cpp", []],
            ["boot.s", []],
        ],
        { std: "c++20" },
    );

    const run = await runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Formatted 1 file ");
    expect(ws.read("boot.s")).toBe("    mov r0, r1\n");
});

test.skipIf(process.platform === "win32")(
    "a symlinked source keeps its link",
    async ({ session }) => {
        const ws = session.tmpdir();
        ws.write(".clang-format", "BasedOnStyle: LLVM\n");
        ws.write("real/x.cpp", UNFORMATTED);
        fs.symlinkSync(ws.path("real/x.cpp"), ws.path("link.cpp"));
        ws.writeCDB(["link.cpp"]);

        const run = await runFormat(ws);
        expect(run.status, `stderr: ${run.stderr}`).toBe(0);
        expect(fs.lstatSync(ws.path("link.cpp")).isSymbolicLink()).toBe(true);
        expect(ws.read("real/x.cpp")).toBe(FORMATTED);
    },
);

test.skipIf(process.platform === "win32")(
    "a link out of the workspace fails the run",
    async ({ session }) => {
        const ws = session.tmpdir();
        const outside = session.tmpdir();
        ws.write(".clang-format", "BasedOnStyle: LLVM\n");
        outside.write("x.cpp", UNFORMATTED);
        fs.symlinkSync(outside.path("x.cpp"), ws.path("link.cpp"));
        ws.writeCDB(["link.cpp"]);

        const explicit = await runFormat(ws, "link.cpp");
        expect(explicit.status).toBe(2);
        expect(explicit.stderr).toContain("links outside the workspace");
        // The automatic set leaves it out without complaint.
        const automatic = await runFormat(ws);
        expect(automatic.status, `stderr: ${automatic.stderr}`).toBe(0);
        expect(outside.read("x.cpp")).toBe(UNFORMATTED);
    },
);

test("a unit inside a system include directory is still its own", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write("src/a.cpp", UNFORMATTED);
    ws.writeCDB(["src/a.cpp"], { extraArgs: ["-isystem", ws.path("src")] });

    const run = await runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Formatted 1 file ");
    expect(ws.read("src/a.cpp")).toBe(FORMATTED);
});

test("an unknown option is a usage error", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = await runFormat(ws, "--jobs", "many");
    expect(run.status).toBe(2);
});
