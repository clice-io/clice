import { test } from "@clice/tools/testing";

// The editor's settings give a value of the wrong type and a rule: none of
// them applies, whichever comes first, and nothing is served.
const RULE = { append: ["-DFOO=0"] };

test("rule before the bad value", async (t) => {
    const s = await t.start({ config: { rules: [RULE], poll_seconds: "yes" } });
    await t.show("guidance", await s.guidance());
    await t.show("main.cpp", s.errors("main.cpp"));
});

test("rule after the bad value", async (t) => {
    const s = await t.start({ config: { poll_seconds: "yes", rules: [RULE] } });
    await t.show("guidance", await s.guidance());
    await t.show("main.cpp", s.errors("main.cpp"));
});
