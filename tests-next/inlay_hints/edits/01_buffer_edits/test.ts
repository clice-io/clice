import { test } from "@clice/tools/testing";

// Hints follow the buffer as the user types, before anything is saved: a
// line added above moves the hints below it, and a new call gets its own.
test("typing above a call", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await t.show("before", s.inlayHints("main.cpp"));
    s.edit("main.cpp", { after: "int frame() {\n", insert: "    auto scale = resize(2, 3);\n" });
    await t.show("after", s.inlayHints("main.cpp"));
});
