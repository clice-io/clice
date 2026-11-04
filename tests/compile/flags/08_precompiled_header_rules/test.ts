import { test } from "@clice/tools/testing";

// A rule edits the commands of the target's units but not its PCH
// builder's: a PCH under the unit's edits is no unit's compile, nothing
// would index it, so the unit parses the header itself under its edits —
// diameter() exists only under the rule's define.

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
