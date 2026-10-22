import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// A named module's mapping leaves, as clice builds its own modules; the
// bare form naming a header unit stays.
test("module files", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -std=c++20 -fmodule-file=m=mod.pcm main.cpp");
    await t.cli("command convert -- missing-clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp");
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fprebuilt-module-path=/tmp main.cpp",
    );
});
