import { test } from "@clice/tools/testing";

// twice(), of a system header, calls helper() twice: the index leaves the
// two calls out until a rule's index_function_body asks for them.
test("left out", async (t) => {
    await t.cli("index");
    await t.cli("query references helper");
});

test("asked for", async (t) => {
    t.disk.write("clice.toml", "[[rules]]\nindex_function_body = true\n");
    await t.cli("index");
    await t.cli("query references helper");
});
