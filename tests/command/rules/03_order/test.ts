import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED = "The command stays a driver command";

// Rules apply in the order the file declares them: a later remove takes
// out what an earlier rule appended, however it is spelled; a remove
// before an append has nothing of it to take.
test({ errors: [MISSING, UNCONVERTED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ later.cpp");
    await t.cli("command convert -- missing-clang++ earlier.cpp");
});
