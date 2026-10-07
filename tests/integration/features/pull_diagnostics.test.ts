/// Pulled diagnostics (textDocument/diagnostic): a client declaring pull
/// support gets an open document's diagnostics only by pulling them, and a
/// pull answers for the buffer as it is once its compile lands.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { MTIME_GRANULARITY, SLOW_SOURCE, sleep, waitUntil } from "@clice/tools/client";
import type { CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

const PULL: proto.ClientCapabilities = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};

const REFRESH = "workspace/diagnostic/refresh";

function messages(diagnostics: proto.Diagnostic[]): string[] {
    return diagnostics.map((diagnostic) =>
        typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value,
    );
}

function mentions(diagnostics: proto.Diagnostic[], name: string): boolean {
    return messages(diagnostics).some((message) => message.includes(name));
}

function refreshes(client: CliceClient, since: number): number {
    return client.serverRequests.slice(since).filter((method) => method === REFRESH).length;
}

async function waitRefresh(client: CliceClient, since: number, description: string) {
    await waitUntil(() => refreshes(client, since) > 0, {
        timeout: 60_000,
        interval: 50,
        description,
    });
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
    expect(mentions(await client.pullDiagnostics(uri), "second")).toBe(true);
    // The client pulls after its own edits: no refresh is owed for them.
    expect(refreshes(client, marker)).toBe(0);
    expect(client.publishCount(uri)).toBe(0);
});

test("edit mid-pull answers the new text", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW_SOURCE + "int a = first;\n");
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("slow.cpp");
    const pending = client.pullDiagnostics(uri);
    // Answered once the pull waits on its compile: the edit lands mid-parse.
    await client.stats();
    client.change(uri, 1, SLOW_SOURCE + "int a = second;\n");

    const diagnostics = await pending;
    expect(mentions(diagnostics, "second")).toBe(true);
    expect(mentions(diagnostics, "first")).toBe(false);
}, 300_000);

test("cancelled pull answers at once", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW_SOURCE);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("slow.cpp");
    const source = new proto.CancellationTokenSource();
    const pending = client.pullDiagnostics(uri, source.token);
    await client.stats();
    source.cancel();
    // The next pull still waits on the compile the cancelled one left.
    let answered = false;
    const next = client.pullDiagnostics(uri).then((diagnostics) => {
        answered = true;
        return diagnostics;
    });
    await expect(pending).rejects.toMatchObject({ code: proto.LSPErrorCodes.RequestCancelled });
    expect(answered).toBe(false);
    expect(await next).toEqual([]);
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
    await waitRefresh(client, marker, "diagnostic refresh after the header changed");
    expect(mentions(await client.pullDiagnostics(uri), "missing")).toBe(true);
    expect(client.publishCount(uri)).toBe(0);
});

test("repeated setup failure stays quiet", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    // Fails before parsing on every attempt, and never settles.
    workspace.writeEntries([["main.cpp", ["--target=bogus-unknown-none"]]]);
    await client.initialize(workspace, { capabilities: PULL });

    const [uri] = client.open("main.cpp");
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    const marker = client.serverRequests.length;
    await client.hoverAt(uri, 0, 4);
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    expect(refreshes(client, marker)).toBe(0);
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

test.skipIf(process.platform === "win32")(
    "second name pulls its own answer",
    async ({ session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("real/main.cpp", "int main() { return missing; }\n");
        fs.symlinkSync(workspace.path("real"), workspace.path("link"));
        workspace.writeCDB(["real/main.cpp"]);
        await client.initialize(workspace, { capabilities: PULL });

        const [first] = client.open("real/main.cpp");
        const [second] = client.open("link/main.cpp");
        expect(mentions(await client.pullDiagnostics(second), "missing"), "equal texts share").toBe(
            true,
        );

        let marker = client.serverRequests.length;
        client.change(first, 1, "int main() { return 0; }\n");
        await waitRefresh(client, marker, "refresh once the texts part");
        expect(messages(await client.pullDiagnostics(second))).toEqual([
            expect.stringContaining("also open as"),
        ]);

        marker = client.serverRequests.length;
        client.close(first);
        await waitRefresh(client, marker, "refresh once the second name takes over");
        expect(mentions(await client.pullDiagnostics(second), "missing")).toBe(true);
        expect(client.publishCount(first) + client.publishCount(second)).toBe(0);
    },
);
