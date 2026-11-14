import { test } from "@clice/tools/testing";

test("editor", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.errors("main.cpp"));
});

// `clice lint` says the same of the import it cannot take.
test("lint", async (t) => {
    await t.cli("lint --jobs 1");
});
