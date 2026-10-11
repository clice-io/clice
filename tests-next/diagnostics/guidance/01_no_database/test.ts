import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("without a database", s.diagnostics("main.cpp"), { parts: ["link"] });
    await t.show("guidance", await s.guidance());
    t.disk.database({ "main.cpp": [] });
    await s.restart();
    await t.show("with a database", s.diagnostics("main.cpp"), { parts: ["link"] });
});
