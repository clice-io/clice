/// SIGTERM drives the same graceful shutdown as the LSP exit notification.
///
/// An editor teardown or system stop delivers SIGTERM, not an LSP shutdown
/// handshake; without a handler the server dies before its shutdown save and
/// the next startup treats the cache as garbage.

import * as fs from "node:fs";
import * as path from "node:path";
import { expect, test } from "../fixtures.ts";

test.skipIf(process.platform === "win32")("sigterm runs shutdown save", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.pinCacheDir();
    workspace.write("main.cpp", "int add(int a, int b) { return a + b; }\n");
    workspace.writeCDB(["main.cpp"]);

    const client = session.spawn(workspace);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(uri);

    // The manifest write after this removal can only come from the shutdown
    // checkpoint (the periodic and commit-count triggers keep their own
    // schedules), so its existence after exit proves the SIGTERM took the
    // graceful path — the exit-code-0 gate inside terminate() proves it
    // completed.
    const manifest = path.join(workspace.cacheRoot(), "manifest.json");
    fs.rmSync(manifest, { force: true });
    await client.terminate();
    expect(fs.existsSync(manifest), "SIGTERM must run the shutdown save").toBe(true);
});

test("server exits on client eof", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);

    const client = session.spawn(workspace);
    await client.initialize(workspace);

    // The server must notice EOF and exit cleanly instead of idling
    // forever on a still-armed watcher handle.
    client.disconnect();
    await client.assertExitedCleanly();
});
