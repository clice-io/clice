import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// The compiler the database names cannot be asked for its toolchain: the
// unit still indexes, from clice's own reading of its command.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("index");
});
