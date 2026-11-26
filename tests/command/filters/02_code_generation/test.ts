import { test } from "@clice/tools/testing";

const UNCONVERTED = "The command stays a driver command";

// Options that only shape the generated code leave; -fPIC and the stack
// protector stay, for the macros they define.
test("codegen", { errors: [UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fPIC -fno-omit-frame-pointer -fstack-protector-strong -fdata-sections -ffunction-sections -fcolor-diagnostics main.cpp",
    );
});

// What only optimization, hardening, the linker, the backend and the
// sanitizers' runtimes read leaves; what predefines macros or sets the
// floating-point semantics Sema evaluates with stays: -O2, fast math,
// control-flow protection, trapping math, the sanitizer itself.
test("optimization and hardening", { errors: [UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -O2 -ffast-math -fcf-protection=full -ftrapping-math -fsanitize=address -fno-plt -fno-strict-aliasing -fvisibility-inlines-hidden -ftrivial-auto-var-init=zero -fstack-clash-protection -fsanitize-recover=address -fprofile-sample-use=main.prof -Wl,--gc-sections -fmessage-length=0 main.cpp",
    );
    await t.cli(
        "command convert -- clang-cl-missing.exe /std:c++20 /O2 /GS- /GL /Gy /guard:cf /Qspectre /bigobj /MP main.cpp",
    );
});

// The LTO modes clang takes stay; g++'s job count goes.
test("lto", { errors: [UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -flto main.cpp");
    await t.cli("command convert -- missing-clang++ -flto=thin main.cpp");
    await t.cli("command convert -- missing-clang++ -flto=auto main.cpp");
    await t.cli("command convert -- missing-g++ -flto=jobserver main.cpp");
    await t.cli("command convert -- missing-g++ -flto=8 main.cpp");
});
