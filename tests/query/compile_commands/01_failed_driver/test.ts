import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// The compiler the database names cannot be asked for its toolchain: the
// unit still indexes from its command as written, and the command answer
// says why it is not the compiler's own.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("index");
    await t.cli("query compile-command main.cpp");
});
