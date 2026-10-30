import { test, type Serve, type TestContext } from "@clice/tools/testing";

/// Rewrite clice.toml with other content, once the server read it: it reads
/// its configuration when the client says it is initialized, and an edit
/// before then is simply what it reads.
async function edit(t: TestContext, s: Serve): Promise<void> {
    await s.sync();
    t.disk.write("clice.toml", '[[rules]]\nappend = ["-DEDITED"]\n');
}

// The server asks a client that can to restart it.
test("the editor restarts", async (t) => {
    const s = await t.start();
    await edit(t, s);
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});

// A client that cannot restart the server sees why it should, on the file.
test("a client without restarts", async (t) => {
    const s = await t.start({ capabilities: {} });
    await edit(t, s);
    await s.poll("workspace");
    await s.sync();
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("asked", s.restartRequests());
});

// Polling is off, as in every test: the server looks at its configuration
// file on its own nonetheless.
test("polling off", async (t) => {
    const s = await t.start();
    await edit(t, s);
    await t.show("asked", await s.restarted());
});

// A save that keeps the content changes nothing.
test("the same content", async (t) => {
    const s = await t.start();
    await s.sync();
    t.disk.touch("clice.toml", new Date(Date.now() + 10_000));
    await s.poll("workspace");
    // A request the look asked for goes out before the sync answers.
    await s.sync();
    await t.show("asked", s.restartRequests());
});
