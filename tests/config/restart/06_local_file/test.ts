import { test } from "@clice/tools/testing";

// The local file applies beside clice.toml: one written, changed or deleted
// while the server runs restarts it.
test("written", async (t) => {
    t.disk.write("clice.toml", "[[rules]]\nbackground_index = false\n");
    const s = await t.start();
    await s.sync();
    t.disk.write(".clice/local.toml", '[[rules]]\nappend = ["-DEDITED"]\n');
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});

test("deleted", async (t) => {
    t.disk.write(".clice/local.toml", '[[rules]]\nappend = ["-DEDITED"]\n');
    const s = await t.start();
    await s.sync();
    t.disk.rm(".clice/local.toml");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});
