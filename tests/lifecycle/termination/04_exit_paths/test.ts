import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// How a server ends besides the shutdown and exit every test ends with: an
// exit with no shutdown before it, as LSP asks, ends it with code 1; a
// SIGTERM, as an editor or a session manager sends, ends it at once.

test("exit without shutdown", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    // The exit alone, which the teardown never sends.
    await s.client.sendNotification(proto.ExitNotification.type);
    const code = await s.client.exited;
    s.client.dispose();
    await t.show("exit code", code ?? -1);
});

test("SIGTERM", { except: { win32: "Windows has no SIGTERM to send a process" } }, async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    s.client.child.kill("SIGTERM");
    const code = await s.client.exited;
    s.client.dispose();
    await t.show("exit", { code: code ?? -1, signal: s.client.child.signalCode ?? "" });
});
