/// Pulled diagnostics (textDocument/diagnostic): a client declaring pull
/// support gets an open document's diagnostics only by pulling them, and a
/// pull answers for the buffer as it is once its compile lands.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import type { CliceClient } from "@clice/tools/client";
import { expect, serve } from "../../fixtures.ts";

const PULL: proto.ClientCapabilities = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};

const REFRESH = "workspace/diagnostic/refresh";

/// A pulling client: the serve fixture's server pushes, so this one is a
/// server of its own on the case's workspace.
async function pulling(s: Serve): Promise<CliceClient> {
    await s.stop();
    return s.session.spawn(s.workspace).initialize(s.workspace, { capabilities: PULL });
}

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

serve.files({ "main.cpp": "int main() { return missing; }\n" })(
    "client capability picks the model",
    async ({ s }) => {
        expect(s.client.initResult?.capabilities.diagnosticProvider).toBeUndefined();
        expect(await s.errors("main.cpp")).toHaveLength(1);
        expect((await s.counts()).files["main.cpp"]?.publish).toBeGreaterThan(0);

        const client = await pulling(s);
        expect(client.initResult?.capabilities.diagnosticProvider).toEqual({
            interFileDependencies: false,
            workspaceDiagnostics: false,
        });
        const [uri] = client.open("main.cpp");
        expect(mentions(await client.pullDiagnostics(uri), "missing")).toBe(true);
        expect(client.publishCount(uri)).toBe(0);
    },
);

serve.files({ "main.cpp": "int main() { return first; }\n" })(
    "pull follows edits",
    async ({ s }) => {
        const client = await pulling(s);
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
    },
);

serve.files({ "main.cpp": "int a = first;\n" })(
    "edit mid-pull answers the new text",
    async ({ s }) => {
        const client = await pulling(s);
        const uri = s.uri("main.cpp");
        const hold = await client.hold("compile", uri);
        client.open("main.cpp");
        const pending = client.pullDiagnostics(uri);
        // The pull waits on its compile: the edit lands mid-parse.
        await client.parkedBy(hold);
        client.change(uri, 1, "int a = second;\n");
        await client.release(hold);

        const diagnostics = await pending;
        expect(mentions(diagnostics, "second")).toBe(true);
        expect(mentions(diagnostics, "first")).toBe(false);
    },
);

serve.files({ "main.cpp": "int main() { return 0; }\n" })(
    "cancelled pull answers at once",
    async ({ s }) => {
        const client = await pulling(s);
        const uri = s.uri("main.cpp");
        const hold = await client.hold("compile", uri);
        client.open("main.cpp");
        const source = new proto.CancellationTokenSource();
        const pending = client.pullDiagnostics(uri, source.token);
        await client.parkedBy(hold);
        source.cancel();
        // The next pull still waits on the compile the cancelled one left.
        let answered = false;
        const next = client.pullDiagnostics(uri).then((diagnostics) => {
            answered = true;
            return diagnostics;
        });
        await expect(pending).rejects.toMatchObject({ code: proto.LSPErrorCodes.RequestCancelled });
        expect(answered).toBe(false);
        await client.release(hold);
        expect(await next).toEqual([]);
    },
);

serve.files({
    "header.h": "inline int value() { return 1; }\n",
    "main.cpp": '#include "header.h"\nint main() { return value(); }\n',
})("recompile of same text refreshes", async ({ s }) => {
    const client = await pulling(s);
    const [uri] = client.open("main.cpp");
    expect(await client.pullDiagnostics(uri)).toEqual([]);

    s.disk.write("header.h", "inline int value() { return missing; }\n");
    const marker = client.serverRequests.length;
    // Any request recompiles the document; its pulled answer went stale
    // with no edit to make the client pull again.
    await client.hoverAt(uri, 1, 22);
    await client.sync();
    expect(
        refreshes(client, marker),
        "diagnostic refresh after the header changed",
    ).toBeGreaterThan(0);
    expect(mentions(await client.pullDiagnostics(uri), "missing")).toBe(true);
    expect(client.publishCount(uri)).toBe(0);
});

// Fails before parsing on every attempt, and never settles.
serve.files(
    { "main.cpp": "int main() { return 0; }\n" },
    { manifest: { units: { "main.cpp": ["--target=bogus-unknown-none"] } } },
)("repeated setup failure stays quiet", async ({ s }) => {
    const client = await pulling(s);
    const [uri] = client.open("main.cpp");
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    const marker = client.serverRequests.length;
    await client.hoverAt(uri, 0, 4);
    expect(await client.pullDiagnostics(uri)).toEqual([]);
    await client.sync();
    expect(refreshes(client, marker)).toBe(0);
});

serve.files({ "main.cpp": "int main() { return missing; }\n" })(
    "closed document pulls empty",
    async ({ s }) => {
        const client = await pulling(s);
        const [uri] = client.open("main.cpp");
        expect(mentions(await client.pullDiagnostics(uri), "missing")).toBe(true);
        client.close(uri);
        expect(await client.pullDiagnostics(uri)).toEqual([]);
        expect(client.publishCount(uri)).toBe(0);
    },
);

if (process.platform !== "win32") {
    serve.files({ "real/main.cpp": "int main() { return missing; }\n" })(
        "second name pulls its own answer",
        async ({ s }) => {
            fs.symlinkSync(s.workspace.path("real"), s.workspace.path("link"));
            const client = await pulling(s);

            const [first] = client.open("real/main.cpp");
            const [second] = client.open("link/main.cpp");
            expect(
                mentions(await client.pullDiagnostics(second), "missing"),
                "equal texts share",
            ).toBe(true);

            let marker = client.serverRequests.length;
            client.change(first, 1, "int main() { return 0; }\n");
            await client.sync();
            expect(refreshes(client, marker), "refresh once the texts part").toBeGreaterThan(0);
            expect(messages(await client.pullDiagnostics(second))).toEqual([
                expect.stringContaining("also open as"),
            ]);

            marker = client.serverRequests.length;
            client.close(first);
            await client.sync();
            expect(
                refreshes(client, marker),
                "refresh once the second name takes over",
            ).toBeGreaterThan(0);
            expect(mentions(await client.pullDiagnostics(second), "missing")).toBe(true);
            expect(client.publishCount(first) + client.publishCount(second)).toBe(0);
        },
    );
}
