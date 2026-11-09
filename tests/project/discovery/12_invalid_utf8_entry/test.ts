import { test } from "@clice/tools/testing";

// The database is Latin-1, as a build writes a legacy -D value: its é is no
// UTF-8. Only a.cpp's entry holds it; b.cpp's is plain ASCII.
test("index", async (t) => {
    await t.cli("index");
});

// The editor is told once how many entries were skipped, the log naming
// each.
test("server", async (t) => {
    const s = await t.start();
    await s.sync();
    await t.show("guidance", await s.guidance());
});
