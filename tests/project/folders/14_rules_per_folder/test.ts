import { test } from "@clice/tools/testing";

test("rules stay in their folder", async (t) => {
    // alpha's clice.toml appends ALPHA_RULE; beta has no rules.
    const s = await t.start({ folders: ["alpha", "beta"] });
    await t.show("alpha", s.errors("alpha/main.cpp"));
    await t.show("beta", s.errors("beta/main.cpp"));
});
