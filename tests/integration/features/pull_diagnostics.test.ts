/// Pulled diagnostics (textDocument/diagnostic): a client declaring pull
/// support gets an open document's diagnostics only by pulling them, and a
/// pull answers for the buffer as it is once its compile lands.

import * as proto from "vscode-languageserver-protocol";
import { MTIME_GRANULARITY, sleep, waitUntil } from "@clice/tools/client";
import type { CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

const PULL: proto.ClientCapabilities = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};

// Slow to parse on any hardware, so an edit lands while a pull still waits
// on the compile.
const SLOW = Array.from({ length: 200_000 }, (_, i) => `int v${i};`).join("\n") + "\n";
const EDIT_SUPERSEDE_DELAY = 300;

const REFRESH = "workspace/diagnostic/refresh";

function mentions(diagnostics: proto.Diagnostic[], name: string): boolean {
    return diagnostics.some((diagnostic) =>
        (typeof diagnostic.message === "string"
            ? diagnostic.message
            : diagnostic.message.value
        ).includes(name),
    );
}

function refreshes(client: CliceClient, since: number): number {
    return client.serverRequests.slice(since).filter((method) => method === REFRESH).length;
}

test("client capability picks the model", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("main.cpp", "int main() { return missing; }\n");
    workspace.writeCDB(["main.cpp"]);

    const pulling = await session.spawn(workspace).initialize(workspace, { capabilities: PULL });
    expect(pulling.initResult?.capabilities.diagnosticProvider).toEqual({
        interFileDependencies: false,
        workspaceDiagnostics: false,
    });
    const [uri] = pulling.open("main.cpp");
    expect(mentions(await pulling.pullDiagnostics(uri), "missing")).toBe(true);
    await pulling.hoverAt(uri, 0, 4);
    expect(pulling.publishCount(uri)).toBe(0);
    await pulling.shutdown();

    const pushed = await session.spawn(workspace).initialize(workspace);
    expect(pushed.initResult?.capabilities.diagnosticProvider).toBeUndefined();
    const [pushedUri] = await pushed.openAndWait("main.cpp");
    pushed.assertHasErrors(pushedUri);
});

test("pull follows edits", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return first; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("main.cpp");
    expect(mentions(await client.pullDiagnostics(uri), "first")).toBe(true);

    const marker = client.serverRequests.length;
    client.change(uri, 1, "int main() { return 0; }\n");
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    client.change(uri, 2, "int main() { return second; }\n");
    const second = await client.pullDiagnostics(uri);
    expect(mentions(second, "second")).toBe(true);
    expect(mentions(second, "first")).toBe(false);
    // The client pulls after its own edits: no refresh is owed for them.
    expect(refreshes(client, marker)).toBe(0);
    expect(client.publishCount(uri)).toBe(0);
});

test("edit mid-pull answers the new text", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW + "int a = first;\n");
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("slow.cpp");
    const pending = client.pullDiagnostics(uri);
    await sleep(EDIT_SUPERSEDE_DELAY);
    client.change(uri, 1, SLOW + "int a = second;\n");

    const diagnostics = await pending;
    expect(mentions(diagnostics, "second")).toBe(true);
    expect(mentions(diagnostics, "first")).toBe(false);
}, 300_000);

test("cancelled pull answers at once", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("slow.cpp");
    const source = new proto.CancellationTokenSource();
    const pending = client.pullDiagnostics(uri, source.token);
    await sleep(EDIT_SUPERSEDE_DELAY);
    source.cancel();
    await expect(pending).rejects.toMatchObject({ code: proto.LSPErrorCodes.RequestCancelled });
    expect(await client.pullDiagnostics(uri)).toEqual([]);
}, 300_000);

test("recompile of same text refreshes", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("header.h", "inline int value() { return 1; }\n");
    workspace.write("main.cpp", '#include "header.h"\nint main() { return value(); }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("main.cpp");
    expect(await client.pullDiagnostics(uri)).toEqual([]);

    await sleep(MTIME_GRANULARITY);
    workspace.write("header.h", "inline int value() { return missing; }\n");
    const marker = client.serverRequests.length;
    // Any request recompiles the document; its pulled answer went stale
    // with no edit to make the client pull again.
    await client.hoverAt(uri, 1, 22);
    await waitUntil(() => refreshes(client, marker) > 0, {
        timeout: 10_000,
        interval: 50,
        description: "diagnostic refresh after the header changed",
    });
    expect(mentions(await client.pullDiagnostics(uri), "missing")).toBe(true);
    expect(client.publishCount(uri)).toBe(0);
});

test("closed document pulls empty", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return missing; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("main.cpp");
    expect(mentions(await client.pullDiagnostics(uri), "missing")).toBe(true);
    client.close(uri);
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    expect(client.publishCount(uri)).toBe(0);
});
