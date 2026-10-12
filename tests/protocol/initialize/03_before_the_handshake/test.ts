import { refusal, test } from "@clice/tools/testing";

// Document notifications an editor sends before its initialize: an open is
// kept and served once the handshake is done; a close takes it back, with
// no diagnostics pushed ahead of the initialize reply.

test("an open is served", async (t) => {
    const s = await t.spawn();
    s.open("main.cpp", { pull: false });
    await s.initialize();
    await t.show("hover", s.hover(t.marker("add")));
    await t.show("pushes", s.pushes("main.cpp"));
});

test("a close takes it back", async (t) => {
    const s = await t.spawn();
    s.open("main.cpp", { pull: false });
    s.close("main.cpp");
    await s.initialize();
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("hover", await refusal(s.hover(t.marker("add"))));
    await t.show("diagnostics after an open", s.diagnostics("main.cpp"));
});
