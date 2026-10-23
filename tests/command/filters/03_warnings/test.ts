import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// Warning options stay, also those flang has spellings of.
test("clang warnings", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -Wall -Wextra -Wconversion main.cpp");
    await t.cli("command convert -- missing-clang-cl /W4 -Wextra /c main.cpp");
    await t.cli("command convert -- missing-clang++ -Wlogical-op main.cpp");
});

// A GCC command's warnings clang does not know leave; those it knows
// stay, in every form clang reads.
test("gcc warnings", { errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli(
        "command convert -- missing-g++ -Wall -Wlogical-op -Wno-useless-cast -Werror=duplicated-cond -Wformat=2 -Wno-error=unused-variable -Werror -Wfatal-errors main.cpp",
    );
});
