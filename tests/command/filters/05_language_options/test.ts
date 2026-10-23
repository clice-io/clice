import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// What decides how the code compiles stays: the standard, the language
// switches and the warnings.
test("kept", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp",
    );
    await t.cli("command convert -- missing-clang++ -std=c++20 -Wall -Werror main.cpp");
});

// Entries alike but for their macros keep each its own.
test("own macros", { errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
