import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["link", "tags"] });
    await t.show("guidance", await s.guidance());
});
