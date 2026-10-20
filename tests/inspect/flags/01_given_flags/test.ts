import { test } from "@clice/tools/testing";

// `clice inspect --flags` compiles the file with the flags given instead of
// its project's command: a response file is read from the file's directory,
// wherever inspect runs, and a header compiles as C++.

test(async (t) => {
    const runs = [
        ["src/main.cpp", '["@args.rsp"]'],
        ["src/shapes.h", '["-std=c++20"]'],
        // The driver makes no clang job of an assembler source.
        ["src/main.cpp", '["-x", "assembler"]'],
        ["src/main.cpp", "[]"],
        ["src/main.cpp", '{"std": 20}'],
    ] as const;
    for (const [file, flags] of runs) {
        await t.cli(["inspect", "semantic-tokens", file, "--flags", flags]);
    }
    await t.cli([
        "inspect",
        "semantic-tokens",
        "src/main.cpp",
        "--flags",
        '["-std=c++20"]',
        "--config-overlay",
        '{"no_such_section": {}}',
    ]);
});
