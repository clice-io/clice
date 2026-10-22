import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// Options that only shape the generated code leave; -fPIC and the stack
// protector stay, for the macros they define.
test("codegen", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fPIC -fno-omit-frame-pointer -fstack-protector-strong -fdata-sections -ffunction-sections -fcolor-diagnostics -g main.cpp",
    );
});

// The LTO modes clang takes stay; g++'s job count goes.
test("lto", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -flto main.cpp");
    await t.cli("command convert -- missing-clang++ -flto=thin main.cpp");
    await t.cli("command convert -- missing-clang++ -flto=auto main.cpp");
    await t.cli("command convert -- missing-g++ -flto=jobserver main.cpp");
    await t.cli("command convert -- missing-g++ -flto=8 main.cpp");
});
