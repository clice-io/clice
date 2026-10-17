import { test } from "@clice/tools/testing";

// A rule edits the commands of the target's units but not its PCH
// builder's: clice builds the PCH under the edits of the unit loading it,
// or the PCH would hold another header than the unit parses — diameter()
// exists only under the rule's define.

test("a rule's define reaches the pch", async (t) => {
    const s = await t.start({
        config: {
            project: { enable_indexing: false },
            rules: [{ patterns: ["src/**"], append: ["-DSHAPES_METRIC"] }],
        },
    });
    await t.show("main.cpp", s.diagnostics("src/main.cpp"));
    await t.show("width", s.definition(t.marker("width")));
});
