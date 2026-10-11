import { test } from "@clice/tools/testing";

test("hover alone pauses", async (t) => {
    const s = await t.start({ crash: { request: "query:Hover", file: "main.cpp" } });
    const add = t.marker("add");
    s.open("main.cpp");
    await s.sync();
    await t.show("hover", s.hover(add));
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("hover while paused", s.hover(add));
    await t.show("highlights", s.highlights(add));
    await t.show("crashes", await s.crashes());
    s.save("main.cpp");
    await t.show("hover after a save", s.hover(add));
    await t.show("crashes after a save", await s.crashes());
});
