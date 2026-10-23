import { test } from "@clice/tools/testing";

// A directive takes no call's arguments: its line answers no signatures,
// and asks no compile for them.
test("an include line", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const park = await s.park("signatureHelp", "main.cpp", { at: "worker" });
    await t.show("include", s.signatureHelp(t.marker("include")));
    await park.release();
});
