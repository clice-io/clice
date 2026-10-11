import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// A file that only compiles inside its includer (an X-macro list) has no
// command of its own to show; a file of the same suffix that stands alone
// does.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("index");
    await t.cli("query --method compileCommand --path list.def");
    await t.cli("query --method compileCommand --path lone.def");
});
