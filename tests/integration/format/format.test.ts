import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function runFormat(ws: Workspace, ...args: string[]) {
    return spawnSync(cliceExecutable(), ["format", ...args, "--workspace", ws.root], {
        encoding: "utf8",
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

test("formats the build's own files in place", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Formatted 3 files");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + FORMATTED);
    expect(ws.read("a.cpp")).toContain(FORMATTED);
    expect(ws.read("b.cpp")).toBe('#include "lib.h"\nint main() { return 0; }\n');
    // Neither the excluded directory nor a system header is touched.
    expect(ws.read("vendor/vendored.h")).toBe("#pragma once\n" + UNFORMATTED);
    expect(ws.read("sys/system.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("check reports the files needing formatting", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const dirty = runFormat(ws, "--check");
    expect(dirty.status, `stderr: ${dirty.stderr}`).toBe(1);
    expect(dirty.stdout).toContain("3 need formatting");
    expect(dirty.stderr).toContain("lib.h:2:");
    expect(dirty.stderr).toContain("clang-format-violations");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);

    expect(runFormat(ws).status).toBe(0);
    const clean = runFormat(ws, "--check");
    expect(clean.status, `stderr: ${clean.stderr}`).toBe(0);
    expect(clean.stdout).toContain("all formatted");
});

test("paths narrow the set or add a file", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("loose.cpp", UNFORMATTED);

    // A file the build does not know is formatted when named; the others
    // stay as they are.
    const one = runFormat(ws, "loose.cpp");
    expect(one.status, `stderr: ${one.stderr}`).toBe(0);
    expect(one.stdout).toContain("Formatted 1 file ");
    expect(ws.read("loose.cpp")).toBe(FORMATTED);
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);

    // A directory narrows the build's own files to those under it; the
    // rules still apply inside it.
    const dir = runFormat(ws, "vendor");
    expect(dir.status, `stderr: ${dir.stderr}`).toBe(0);
    expect(dir.stdout).toContain("No files to format");
    expect(ws.read("vendor/vendored.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("a missing clang-format fails the run", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = runFormat(ws, "--clang-format", ws.path("no-such-clang-format"));
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("clang-format not found");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("a database above the workspace is not a build tree", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write(
        "src/clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n[[rules]]\ncompile_commands = [".."]\n',
    );
    ws.write("src/a.cpp", UNFORMATTED);
    ws.writeCDB(["src/a.cpp"]);

    const run = spawnSync(cliceExecutable(), ["format", "--check", "--workspace", ws.path("src")], {
        encoding: "utf8",
        timeout: 120_000,
    });
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("Checked 1 file ");
});

test("a missing file argument fails the run", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = runFormat(ws, "missing.cpp");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("missing.cpp: no such file");
});

test("a non-source file argument fails the run", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("README.md", "# notes\n");

    const run = runFormat(ws, "README.md");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("README.md: not a C-family source file");
    expect(ws.read("README.md")).toBe("# notes\n");
});

test("an invalid log level is a usage error", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = runFormat(ws, "--log-level", "loud");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("unknown log level");
});

test("a workspace that does not exist fails the run", ({ session }) => {
    const ws = session.tmpdir();
    const run = spawnSync(
        cliceExecutable(),
        ["format", "--check", "--workspace", ws.path("missing")],
        { encoding: "utf8", timeout: 120_000 },
    );
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("not a directory");
});

test("a broken configuration fails the run", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("clice.toml", "[[rules\npatterns = [\n");

    const run = runFormat(ws);
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("clice.toml");
    expect(ws.read("lib.h")).toBe("#pragma once\n" + UNFORMATTED);
});

test("a broken compilation database fails the run", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    ws.write("compile_commands.json", "{");

    const run = runFormat(ws, "--check");
    expect(run.status).toBe(2);
    expect(run.stderr).toContain("compilation database could not be loaded");
});

test("only C-family units are formatted", ({ session }) => {
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

    const run = runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Formatted 1 file ");
    expect(ws.read("boot.s")).toBe("    mov r0, r1\n");
});

test.skipIf(process.platform === "win32")("a symlinked source keeps its link", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write("real/x.cpp", UNFORMATTED);
    fs.symlinkSync(ws.path("real/x.cpp"), ws.path("link.cpp"));
    ws.writeCDB(["link.cpp"]);

    const run = runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(fs.lstatSync(ws.path("link.cpp")).isSymbolicLink()).toBe(true);
    expect(ws.read("real/x.cpp")).toBe(FORMATTED);
});

test.skipIf(process.platform === "win32")(
    "a link out of the workspace fails the run",
    ({ session }) => {
        const ws = session.tmpdir();
        const outside = session.tmpdir();
        ws.write(".clang-format", "BasedOnStyle: LLVM\n");
        outside.write("x.cpp", UNFORMATTED);
        fs.symlinkSync(outside.path("x.cpp"), ws.path("link.cpp"));
        ws.writeCDB(["link.cpp"]);

        const explicit = runFormat(ws, "link.cpp");
        expect(explicit.status).toBe(2);
        expect(explicit.stderr).toContain("links outside the workspace");
        // The automatic set leaves it out without complaint.
        const automatic = runFormat(ws);
        expect(automatic.status, `stderr: ${automatic.stderr}`).toBe(0);
        expect(outside.read("x.cpp")).toBe(UNFORMATTED);
    },
);

test("a unit inside a system include directory is still its own", ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clang-format", "BasedOnStyle: LLVM\n");
    ws.write("src/a.cpp", UNFORMATTED);
    ws.writeCDB(["src/a.cpp"], { extraArgs: ["-isystem", ws.path("src")] });

    const run = runFormat(ws);
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Formatted 1 file ");
    expect(ws.read("src/a.cpp")).toBe(FORMATTED);
});

test("an unknown option is a usage error", ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);

    const run = runFormat(ws, "--jobs", "many");
    expect(run.status).toBe(2);
});
