import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// The rules edit the database's command into the one clice compiles with.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("command main.cpp");
    await t.cli("command main.cpp --json");
});
