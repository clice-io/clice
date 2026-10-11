import { test } from "@clice/tools/testing";

// The second include of a.h is skipped by its guard, yet still links to it
// when the links come from the index.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("links", s.documentLinks("main.cpp"));
});
