import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// Appended flags go before the file, so they govern it, and after every
// flag the command has, so they win: over a flag written after the file,
// and over a language selector before it, while one after it still
// governs nothing. An option clice does not know keeps its value.
test({ errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ --output=main.o -D A -D B=0 defined.cpp");
    await t.cli("command convert -- missing-clang++ unknown.cpp");
    await t.cli("command convert -- missing-clang -x c selector.c -x none");
    await t.cli("command convert -- missing-clang -c after.c -std=c11");
});
