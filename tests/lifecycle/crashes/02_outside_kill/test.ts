import { test } from "@clice/tools/testing";

test("nobody is blamed", async (t) => {
    const s = await t.start({ kill: { request: "query:Hover", file: "main.cpp" } });
    const add = t.marker("add");
    s.open("main.cpp");
    await s.sync();
    await t.show("hover", s.hover(add));
    await t.show("pushes", s.pushes("main.cpp"));
});
