import { test } from "@clice/tools/testing";

// Options most commands share: a workspace, a job count; the log level
// every command takes from its environment; and what a command run from a
// subdirectory works on.

test("log level", async (t) => {
    await t.cli("index --jobs 1", { env: { CLICE_LOG_LEVEL: "bogus" } });
    await t.cli("index --jobs 1", { env: { CLICE_LOG_LEVEL: "error" } });
});

test("a job count by its short name", async (t) => {
    await t.cli("index -j 1");
});

test("a missing workspace", async (t) => {
    for (const command of ["query project-files", "format --check", "lint", "index", "scan"]) {
        await t.cli(`${command} --workspace missing`);
    }
    // What the runs left where the workspace was named.
    await t.show("left behind", t.disk.list("missing/.clice/*"));
});

// Run from a subdirectory, a command works on the project above it, and its
// paths are relative to where it runs.
test("a subdirectory", async (t) => {
    t.disk.write("sub/.keep", "");
    await t.cli("format --check ../main.cpp", { cwd: "sub" });
    await t.cli("index --jobs 1", { cwd: "sub" });
    await t.cli("index file ../main.cpp", { cwd: "sub" });
});
