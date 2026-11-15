import { test } from "@clice/tools/testing";

// The open file's compile names the header's member, which no background
// index reached.
test(async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    s.open("main.cpp");
    await s.errors("main.cpp");
    await t.show("area", s.rename(t.marker("area"), "surface"));
});
