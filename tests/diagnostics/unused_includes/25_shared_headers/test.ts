import { test } from "@clice/tools/testing";

// The file opens after the index has every header: its run again still
// reads what the headers name.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cpp");
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
});
