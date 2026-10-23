import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// The compiler the database names cannot be asked for its toolchain: the
// unit still indexes, clang's own driver reading its command.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("index");
});
