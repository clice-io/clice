import { test } from "@clice/tools/testing";

// The editor's settings give a value of the wrong type and a rule: none of
// them applies, whichever comes first, and nothing is served. The test
// hooks come from clice.toml.
const RULE = { append: ["-DFOO=0"] };
const INVALID = { errors: ["initializationOptions: invalid type"] };

test("rule before the bad value", INVALID, async (t) => {
    const s = await t.start({ config: { rules: [RULE], project: { enable_indexing: "yes" } } });
    await t.show("guidance", await s.guidance());
    await t.show("main.cpp", s.errors("main.cpp"));
});

test("rule after the bad value", INVALID, async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: "yes" }, rules: [RULE] } });
    await t.show("guidance", await s.guidance());
    await t.show("main.cpp", s.errors("main.cpp"));
});
