import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "|report(1, 2)")));
});
