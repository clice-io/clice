import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver: missing-cc";
const UNRESOLVED = "The compiler could not be queried";

// The command after -- runs in the current directory and converts as a
// database entry for its source would.
test("as an entry", { errors: [MISSING, UNRESOLVED] }, async (t) => {
    await t.cli("command convert -- missing-cc -std=c++20 -c main.cpp -o main.o");
    await t.cli("command convert --json -- missing-cc -std=c++20 -c main.cpp");
});

test("one source a command", { errors: ["convert takes a command compiling one"] }, async (t) => {
    await t.cli("command convert -- missing-cc -c main.cpp other.cpp");
    await t.cli("command convert -- missing-cc -c");
});

test("a database or a command", async (t) => {
    await t.cli("command convert");
    await t.cli("command convert compile_commands.json -- missing-cc -c main.cpp");
    await t.cli("command convert compile_commands.json --json");
});
