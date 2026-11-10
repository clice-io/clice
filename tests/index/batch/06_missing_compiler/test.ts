import { test } from "@clice/tools/testing";

// The compiler the database names cannot be asked for its toolchain: the
// unit still indexes, clang's own driver reading its command.
test(async (t) => {
    await t.cli("index");
});
