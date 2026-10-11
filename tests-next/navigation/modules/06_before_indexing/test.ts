import { test } from "@clice/tools/testing";

// F12 right after opening a file, before the background index has read the
// module interface the name comes from: the interface's index is held back
// so the request lands before it, as it does when a user is quick.
test(async (t) => {
    const s = await t.start();
    const held = await s.park("index", "math.cppm", { at: "reply" });
    s.open("main.cpp");
    await t.show("before the index", s.definition(t.marker("use")));
    await held.release();
    await s.indexed();
    await t.show("after the index", s.definition(t.marker("use")));
});
