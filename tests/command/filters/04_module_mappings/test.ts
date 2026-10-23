import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// A named module's mapping leaves, as clice builds its own modules; the
// bare form naming a header unit stays.
test("module files", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -std=c++20 -fmodule-file=m=mod.pcm main.cpp");
    await t.cli("command convert -- missing-clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp");
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fprebuilt-module-path=/tmp main.cpp",
    );
});
