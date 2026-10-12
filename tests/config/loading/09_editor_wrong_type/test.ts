import { test } from "@clice/tools/testing";

// The editor's settings give a value of the wrong type. The test hooks come
// from clice.toml: the editor's settings carry them too, after the bad value.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: "yes" } } });
    await t.show("guidance", await s.guidance());
});
