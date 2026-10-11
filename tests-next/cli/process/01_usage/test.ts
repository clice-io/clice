import { test } from "@clice/tools/testing";

/// In the order of the root usage.
const COMMANDS = [
    "serve",
    "query",
    "refactor",
    "worker",
    "index",
    "lint",
    "format",
    "inspect",
    "analyze",
    "modularize",
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
