import { test } from "@clice/tools/testing";

// .clice/config.toml cannot be read (a directory of that name): the project
// is broken, and removing it restarts the server, which serves the project
// with the defaults.
test({ errors: ["Invalid clice\\.toml"] }, async (t) => {
    t.disk.write(".clice/config.toml/.keep", "");
    const s = await t.start();
    await s.sync();
    t.disk.rm(".clice/config.toml");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
