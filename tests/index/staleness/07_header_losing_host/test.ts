import { test } from "@clice/tools/testing";

// A header indexed with a borrowed host, whose host then stops including
// it: no unit can host it any more, so a batch run reports it uncovered,
// every run again, until the header is gone.

test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("alpha"));
    // Edited on disk while its buffer is open, the closed header no longer
    // matches its shard: it is indexed again, standalone, main.cpp lending
    // its command. `beta` can only come from that.
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
