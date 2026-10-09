/// Databases nobody declared, from the command line: inspect and batch
/// indexing find the databases a server would load and borrow commands the
/// same way (the server's side is in serve/compilation/discovery).

import * as fs from "node:fs";
import * as path from "node:path";
import { runProcess } from "@clice/tools/client";
import { DATA_DIR } from "@clice/tools/compile-commands";
import { cliceExecutable, expect, test } from "../fixtures.ts";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

test("inspect borrows the same way", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("src/lib.cpp", "int lib() { return 0; }\n");
    workspace.write("src/new.cpp", gated("FEATURE"));
    workspace.writeEntries([["src/lib.cpp", ["-DFEATURE"]]]);
    const run = await runProcess(
        cliceExecutable(),
        ["inspect", "hover", path.join(workspace.root, "src", "new.cpp")],
        { timeout: 120_000 },
    );
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    const output = JSON.parse(run.stdout) as {
        files: Record<string, { diagnostics?: string[] | null }>;
    };
    expect(Object.values(output.files).flatMap((file) => file.diagnostics ?? [])).toEqual([]);
});

test("inspect loads the databases above its inputs", async () => {
    // A directory inspection meets the nested projects' databases the way
    // opening their files would.
    const run = await runProcess(
        cliceExecutable(),
        ["inspect", "hover", path.join(DATA_DIR, "cdb", "nested_projects")],
        { timeout: 120_000 },
    );
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    const output = JSON.parse(run.stdout) as {
        files: Record<string, { diagnostics?: string[] | null }>;
    };
    for (const file of ["group-a/p1/main.cpp", "group-a/p2/main.cpp"]) {
        expect(output.files[file], file).toBeDefined();
        expect(output.files[file]?.diagnostics ?? [], file).toEqual([]);
    }
});

test("batch indexing finds nested projects", async ({ session }) => {
    const workspace = session.tmpdir();
    fs.cpSync(path.join(DATA_DIR, "cdb", "nested_projects"), workspace.root, { recursive: true });
    const run = await runProcess(
        cliceExecutable(),
        ["index", "--workspace", workspace.root, "--workers", "2"],
        { timeout: 120_000 },
    );
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    expect(run.stdout).toContain("Indexed 3 translation units in");
});

test.skipIf(process.platform === "win32")(
    "batch indexing finds a linked build tree",
    async ({ session }) => {
        const workspace = session.tmpdir();
        const outside = session.tmpdir();
        workspace.write("sub/main.cpp", "int main() { return 0; }\n");
        outside.write(
            "build/compile_commands.json",
            JSON.stringify([
                {
                    directory: workspace.path("sub"),
                    file: workspace.path("sub/main.cpp"),
                    arguments: ["clang++", "-c", workspace.path("sub/main.cpp")],
                },
            ]),
        );
        fs.symlinkSync(outside.path("build"), workspace.path("sub/build"));
        const run = await runProcess(
            cliceExecutable(),
            ["index", "--workspace", workspace.root, "--workers", "2"],
            { timeout: 120_000 },
        );
        expect(run.status, `stderr: ${run.stderr}`).toBe(0);
        expect(run.stdout).toContain("Indexed 1 translation unit in");
    },
);
