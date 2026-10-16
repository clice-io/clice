import { test } from "@clice/tools/testing";

/// In the order of the root usage, then worker, which the server spawns
/// and the root usage leaves out.
const COMMANDS = [
    "serve",
    "query",
    "refactor",
    "index",
    "lint",
    "format",
    "inspect",
    "scan",
    "analyze",
    "modularize",
    "worker",
];

test("root usage", async (t) => {
    await t.cli([]);
    await t.cli("--help");
});

test("command help", async (t) => {
    for (const command of COMMANDS) {
        await t.cli([command, "--help"]);
    }
});

// One of each way a command takes operations: words only, a job of its
// own beside them, and one level further down.
test("operation help", async (t) => {
    for (const operation of [
        "query references",
        "index stats",
        "analyze modules",
        "analyze modules edge",
        "inspect hover",
    ]) {
        await t.cli([...operation.split(" "), "--help"]);
    }
});

test("unknown subcommand", async (t) => {
    await t.cli("bogus");
});

// A target an operation takes, missing or one too many.
test("positional arguments", async (t) => {
    for (const command of [
        "index symbol",
        "index file a.h b.h",
        "inspect hover",
        "modularize merge",
        "query references",
    ]) {
        await t.cli(command);
    }
});

// An option that does not parse fails before the help it asks beside it.
test("unknown option beside help", async (t) => {
    await t.cli(["analyze", "modules", "--bogus", "--help"]);
});

test("unknown operation", async (t) => {
    for (const command of ["query", "index", "analyze modules", "modularize"]) {
        await t.cli([...command.split(" "), "bogus"]);
    }
});

test("unknown option", async (t) => {
    for (const command of COMMANDS) {
        await t.cli([command, "--bogus"]);
    }
});
