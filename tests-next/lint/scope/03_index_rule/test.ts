import { test } from "@clice/tools/testing";

// Pins --index beside the rules: a unit kept out of lint and of the index
// is not even parsed; one kept out of lint alone is parsed for the index
// and reports nothing. Either way the run leaves the index whole, so the
// next `clice index` has nothing to do.
test("out of both", async (t) => {
    await t.cli("lint --index --workers 2");
    await t.cli("index --workers 2");
});

test("out of lint alone", async (t) => {
    t.disk.edit("clice.toml", { remove: "index = false\n" });
    await t.cli("lint --index --workers 2");
    await t.cli("index --workers 2");
});
