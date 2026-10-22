import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";
const UNRESOLVED = "of the converted entries could not be resolved";

// What decides how the code compiles stays: the standard, the language
// switches and the warnings.
test("kept", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp",
    );
    await t.cli("command convert -- missing-clang++ -std=c++20 -Wall -Werror main.cpp");
});

// Entries alike but for their macros keep each its own.
test("own macros", { errors: [MISSING, UNRESOLVED] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
