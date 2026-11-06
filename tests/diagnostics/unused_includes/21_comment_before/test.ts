import { test } from "@clice/tools/testing";

// The comment ending before the directive stays when the directive goes.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
    await t.show("fixes", s.codeActions(t.marker("unused"), { only: ["quickfix"] }));
});
