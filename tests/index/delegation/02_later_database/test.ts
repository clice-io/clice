import { test } from "@clice/tools/testing";

// The server started before the project had a database: asked to index
// once one exists, it loads it and indexes.
test(async (t) => {
    const s = await t.start();
    await s.sync();
    await t.cli("index");
    t.disk.database({ "main.cpp": [] });
    await t.cli("index");
    await s.sync();
    await t.show("symbols", s.workspaceSymbols("compute"));
});
