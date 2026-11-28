import { test } from "@clice/tools/testing";

// .clice/clice.toml cannot be read (a directory of that name): the project
// is broken, and removing it restarts the server, which serves the project
// with the defaults.
test(async (t) => {
    t.disk.write(".clice/clice.toml/.keep", "");
    const s = await t.start();
    await s.sync();
    t.disk.rm(".clice/clice.toml");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
