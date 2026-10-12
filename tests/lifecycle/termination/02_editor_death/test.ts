import { spawn } from "node:child_process";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// The server watches the editor process its initialize names: an editor
// that dies while a descendant keeps the server's input open still ends
// the server, before or after a shutdown request.

for (const shutdown of [false, true]) {
    test(shutdown ? "after a shutdown" : "without a shutdown", async (t) => {
        const s = await t.spawn();
        const editor = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"]);
        try {
            // The handshake names another process than the test's own.
            await s.client.sendRequest("initialize", {
                processId: editor.pid,
                rootUri: s.uri(""),
                capabilities: {},
                initializationOptions: { project: { cache_dir: path.join(t.root, ".clice") } },
            });
            await s.client.sendNotification(proto.InitializedNotification.type, {});
            if (shutdown) {
                await s.client.sendRequest(proto.ShutdownRequest.type);
            }
        } finally {
            editor.kill("SIGKILL");
        }
        await s.client.assertExitedCleanly(15_000);
        s.client.dispose();
    });
}
