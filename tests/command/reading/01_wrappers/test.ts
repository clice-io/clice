import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// A compiler launcher leaves, with its own options, in any casing.
test("launchers", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- ccache missing-clang++ -std=c++20 main.cpp");
    await t.cli(
        "command convert -- ccache --set-config cache_dir=/tmp/cc missing-clang++ -std=c++20 main.cpp",
    );
    await t.cli("command convert -- CCACHE.EXE missing-clang++ -std=c++20 main.cpp");
});
