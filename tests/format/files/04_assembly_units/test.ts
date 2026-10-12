import { test } from "@clice/tools/testing";

// The build compiles an assembly file too; only the C-family unit is
// formatted.
test(async (t) => {
    await t.cli("format");
});
