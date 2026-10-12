import { test } from "@clice/tools/testing";

// The interface re-exports a partition no file provides.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.errors("main.cpp"));
    await t.show("a.cppm", s.errors("a.cppm"));
});
