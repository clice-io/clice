import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// Every entry of the file shows, in the order the build ranks them; one
// written as a `command` is split as clice reads it.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("command main.cpp");
    await t.cli("command main.cpp --json");
});
