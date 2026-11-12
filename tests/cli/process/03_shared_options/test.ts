import { test } from "@clice/tools/testing";

// Options most commands share: a workspace, a job count; and what a command
// run from a subdirectory works on.

/// The levels `opening` reads at the start of the lines, each once, sorted.
function levels(text: string, opening: RegExp): string[] {
    const found = text.split(/\r?\n/).map((line) => opening.exec(line)?.[1]);
    return [...new Set(found.filter((level) => level !== undefined))].sort();
}

// A log level the environment names wrong is reported once and ignored.
test("log level", async (t) => {
    await t.cli("index --jobs 1", { env: { CLICE_LOG_LEVEL: "bogus" } });
});

// A log level sets what stderr shows: `error` hides the warning about a field
// clice.toml misspells, which the session log keeps beside its info lines;
// `info` shows the info lines as well.
test("valid log levels", async (t) => {
    t.disk.write("clice.toml", "[project]\nmisspelled = 1\n");
    await t.cli("index --jobs 1", { env: { CLICE_LOG_LEVEL: "error" } });
    const [log] = t.disk.list(".clice/logs/*/index.log");
    await t.show("session log levels", levels(t.disk.read(log ?? ""), /^\[[^\]]*\] \[(\w+)\]/));
    const run = await t.cli("index --jobs 1", { env: { CLICE_LOG_LEVEL: "info" } });
    await t.show("stderr levels", levels(run.stderr, /^(trace|debug|info|warning|error): /));
});

test("a job count by its short name", async (t) => {
    await t.cli("index -j 1");
});

test("a missing workspace", { errors: ["missing: not a directory"] }, async (t) => {
    t.disk.write("partition.json", '{ "modules": [] }');
    for (const command of [
        "query project-files",
        "refactor rename main entry",
        "modularize --partition partition.json --out wrap",
        "format --check",
        "lint",
        "index",
        "scan",
    ]) {
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
