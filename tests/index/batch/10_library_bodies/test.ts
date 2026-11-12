import { test } from "@clice/tools/testing";

// twice(), of a system header, calls helper() twice: the index leaves the
// two calls out until project.index_library_bodies asks for them.
test("left out", async (t) => {
    await t.cli("index");
    await t.cli("query references helper");
});

test("asked for", async (t) => {
    t.disk.write("clice.toml", "[project]\nindex_library_bodies = true\n");
    await t.cli("index");
    await t.cli("query references helper");
});
