import { test } from "@clice/tools/testing";

const COMMANDS = [
    "serve",
    "query",
    "refactor",
    "index",
    "lint",
    "format",
    "inspect",
    "analyze",
    "modularize",
];

test("root usage", async (t) => {
    await t.cli([]);
});

test("unknown subcommand", async (t) => {
    await t.cli("bogus");
});

test("unknown option", async (t) => {
    for (const command of COMMANDS) {
        await t.cli([command, "--bogus"]);
    }
});
