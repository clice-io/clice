import { test } from "@clice/tools/testing";

// A file outside both folders compiles with the default command, whichever
// folder the client listed first: it borrows neither folder's database.
test("alpha first", async (t) => {
    const s = await t.start({ folders: ["alpha", "beta"] });
    await t.show("stray", s.diagnostics("elsewhere/stray.cpp"));
});

test("beta first", async (t) => {
    const s = await t.start({ folders: ["beta", "alpha"] });
    await t.show("stray", s.diagnostics("elsewhere/stray.cpp"));
});
