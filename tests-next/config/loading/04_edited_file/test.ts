import { test } from "@clice/tools/testing";

// The rule gains the macro while the server runs; the file is read at
// startup only.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.diagnostics("main.cpp"));
    const written = t.disk.mtime("clice.toml");
    t.disk.edit("clice.toml", { replace: "-DUNRELATED", with: "-DFROM_TOML" });
    t.disk.touch("clice.toml", new Date(written.getTime() + 10_000));
    await s.poll("workspace");
    await t.show("edited", s.diagnostics("main.cpp"));
    await s.restart();
    await t.show("restarted", s.diagnostics("main.cpp"));
});
