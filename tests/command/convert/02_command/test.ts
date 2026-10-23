import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver: missing-cc";
const UNCONVERTED = "The command stays a driver command";

// The command after -- runs in the current directory and converts as a
// database entry for its source would.
test("as an entry", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-cc -std=c++20 -c main.cpp -o main.o");
    await t.cli("command convert --json -- missing-cc -std=c++20 -c main.cpp");
});

// The source is found as the driver reads the arguments: through a
// language selector, under -working-directory; it keeps its place among
// them, so a selector after it does not govern it.
test("the source in its place", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-cc -x c++ -c tool");
    await t.cli("command convert -- missing-cc -working-directory sub -c main.cpp");
    await t.cli("command convert -- missing-cc -x c++ main.c -x c -c");
    await t.cli("command convert -- missing-cc --driver-mode=cl /c /Tptool");
});

// The command runs in the current directory, not the workspace's.
test("in a subdirectory", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert --workspace .. -- missing-cc -c main.cpp", { cwd: "sub" });
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
