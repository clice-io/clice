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

test("unknown subcommand", async (t) => {
    await t.cli("bogus");
});

test("unknown option", async (t) => {
    for (const command of COMMANDS) {
        await t.cli([command, "--bogus"]);
    }
});
