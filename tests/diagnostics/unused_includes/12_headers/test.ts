import { test } from "@clice/tools/testing";

// A header's includes may be there for the files including it: an open
// header gets no unused-include warning, a source including it does.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
    await t.show("util.h", s.diagnostics("util.h"), { parts: ["tags"] });
});
