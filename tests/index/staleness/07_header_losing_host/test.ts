import { test } from "@clice/tools/testing";

// A header is indexed by the units reading it, never on its own: once its
// only includer stops including it, its entries leave with that unit's
// next run, and a batch run has nothing to report about it.

test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("alpha"));
    // Edited on disk while its buffer is open, the closed header no longer
    // matches its shard: main.cpp, which includes it, indexes it again.
    // `beta` can only come from that.
    await s.diagnostics("a.h");
    t.disk.edit("a.h", {
        after: "alpha() { return 1; }\n",
        insert: "inline int beta() { return 2; }\n",
    });
    s.close("a.h");
    await s.indexed();
    await t.show("symbols after the header changed", s.workspaceSymbols("beta"));
    await s.stop();
    // Offline, the host's command changes and its include of a.h goes.
    t.disk.write("main.cpp", "int app_entry() { return 0; }\n");
    t.disk.database({ "main.cpp": ["-DHOST_V2"] });
    await t.cli("index --jobs 2");
    await t.cli("index --jobs 2");
    t.disk.rm("a.h");
    await t.cli("index --jobs 2");
});
