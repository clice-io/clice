import { test } from "@clice/tools/testing";

// A file whose own index run crashes its worker is not retried: the same
// bytes would crash the next run too. It is indexed again once it changes.

test(async (t) => {
    const s = await t.start({ crash: { request: "tuRun", file: "poison.cpp" } });
    s.open("main.cpp");
    await t.show("sync", { failed: (await s.sync()).failed });
    await t.show("symbols", s.workspaceSymbols("_fn"));
    await t.show("crashes", await s.crashes());
    t.disk.edit("poison.cpp", { replace: "return 1;", with: "return 300;" });
    await t.show("sync after a change", { failed: (await s.sync({ poll: true })).failed });
    await t.show("symbols after a change", s.workspaceSymbols("_fn"));
    await t.show("crashes after a change", await s.crashes());
});
