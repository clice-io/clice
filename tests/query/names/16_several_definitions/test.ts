import { test } from "@clice/tools/testing";

// A C function a weak and a strong definition give is one symbol two
// sources define: a search lists it at each, and its definition answers
// with both.
test(async (t) => {
    await t.cli("index");
    await t.cli("query symbol-search father");
    await t.cli("query definition father");
});
