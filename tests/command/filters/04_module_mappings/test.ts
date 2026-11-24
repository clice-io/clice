import { test } from "@clice/tools/testing";

const UNCONVERTED = "The command stays a driver command";

// A named module's mapping leaves, as clice builds its own modules; the
// bare form naming a header unit stays.
test("module files", { errors: [UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -std=c++20 -fmodule-file=m=mod.pcm main.cpp");
    await t.cli("command convert -- missing-clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp");
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fprebuilt-module-path=/tmp main.cpp",
    );
});

// Clang's header modules, the module files a build of them reads and the
// layering check they enforce leave: clice builds no header module, and a
// file compiles as if no module map named it.
test("header modules", { errors: [UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fmodule-name=//pkg:lib -fmodule-map-file=pkg/lib.cppmap -fmodules-strict-decluse -Wprivate-header main.cpp",
    );
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fmodules-cache-path=cache -fimplicit-module-maps -fbuiltin-module-map main.cpp",
    );
    await t.cli(
        "command convert -- missing-clang++ -std=c++20 -fmodules -fmodule-file=bazel-out/pkg/dep.pcm main.cpp",
    );
});
