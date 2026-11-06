import { test } from "@clice/tools/testing";

// A header the database compiles on its own gets no warning either.
test(async (t) => {
    const s = await t.start();
    s.open("util.h");
    await s.indexed();
    await t.show("util.h", s.diagnostics("util.h"), { parts: ["tags"] });
});
