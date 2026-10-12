import { test } from "@clice/tools/testing";

// A system include directory keeps out the headers the build includes from
// it, never the build's own units.
test(async (t) => {
    await t.cli("format");
});
