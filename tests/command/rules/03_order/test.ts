import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// Rules apply in the order the file declares them: a later remove takes
// out what an earlier rule appended, however it is spelled; a remove
// before an append has nothing of it to take.
test({ errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ later.cpp");
    await t.cli("command convert -- missing-clang++ earlier.cpp");
});
