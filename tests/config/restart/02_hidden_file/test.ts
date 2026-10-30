import { test } from "@clice/tools/testing";

// .clice/config.toml configures a folder without a clice.toml: a change to
// it restarts the server.
test("in use", async (t) => {
    const s = await t.start();
    await s.sync();
    t.disk.write(".clice/config.toml", '[[rules]]\nappend = ["-DEDITED"]\n');
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});

// Beside a clice.toml it configures nothing, and a change to it nothing
// either.
test("beside clice.toml", async (t) => {
    t.disk.write("clice.toml", "[project]\nenable_indexing = false\n");
    const s = await t.start();
    await s.sync();
    t.disk.write(".clice/config.toml", '[[rules]]\nappend = ["-DEDITED"]\n');
    await s.poll("workspace");
    await t.show("asked", s.restartRequests());
});
