import { test } from "@clice/tools/testing";

// A worker killed from outside while it indexes names no file: the run in
// flight is lost, not blamed, and indexed again by a later round.

test(async (t) => {
    const s = await t.start({ kill: { request: "tuRun", file: "file_1.cpp" } });
    s.open("main.cpp");
    await t.show("sync", { failed: (await s.sync()).failed });
    await t.show("symbols", s.workspaceSymbols("func_"));
});
