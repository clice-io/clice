import { test } from "@clice/tools/testing";

// A source removed while no server ran: the next server's first answers
// leave its rows out before any background round looks at the disk.
test(async (t) => {
    await t.cli("index");
    t.disk.rm("b.cpp");
    const s = await t.start({ config: { project: { idle_timeout_ms: 600_000 } } });
    await t.show("symbols", {
        removed: s.workspaceSymbols("only_in_b"),
        kept: s.workspaceSymbols("main"),
    });
});
