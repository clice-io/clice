import { test } from "@clice/tools/testing";

test("borrowed command notes missing includes", async (t) => {
    const s = await t.start();
    t.disk.write("src/new.cpp", '#include "nope.h"\n');
    await t.show("new.cpp", s.diagnostics("src/new.cpp"));
});
