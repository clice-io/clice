/// Pulled diagnostics (textDocument/diagnostic): a client declaring pull
/// support gets an open document's diagnostics only by pulling them, and a
/// pull answers for the buffer as it is once its compile lands.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import type { FileText, Serve, ServeOptions } from "@clice/tools/actions";
import { at, expect, serve, type ServeTest } from "../../fixtures.ts";

const PULL: proto.ClientCapabilities = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};

const REFRESH = "workspace/diagnostic/refresh";

/// Cases whose server is started by a client declaring pull support.
function pulling(files: Record<string, FileText>, options: ServeOptions = {}): ServeTest {
    return serve.files(files, { ...options, launch: { capabilities: PULL } });
}

function messages(diagnostics: proto.Diagnostic[]): string[] {
    return diagnostics.map((diagnostic) =>
        typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value,
    );
}

function mentions(diagnostics: proto.Diagnostic[], name: string): boolean {
    return messages(diagnostics).some((message) => message.includes(name));
}

/// The server's requests to the client so far: the refreshes are among them.
function requested(s: Serve): number {
    return s.client.serverRequests.length;
}

function refreshes(s: Serve, since: number): number {
    return s.client.serverRequests.slice(since).filter((method) => method === REFRESH).length;
}

serve.files({ "main.cpp": "int main() { return missing; }\n" })(
    "client capability picks the model",
    async ({ s }) => {
        expect(s.initResult.capabilities.diagnosticProvider).toBeUndefined();
        expect(await s.errors("main.cpp")).toHaveLength(1);
        expect((await s.counts()).files["main.cpp"]?.publish).toBeGreaterThan(0);

        await s.stop();
        await s.start({ capabilities: PULL });
        expect(s.initResult.capabilities.diagnosticProvider).toEqual({
            interFileDependencies: false,
            workspaceDiagnostics: false,
        });
        s.open("main.cpp", { pull: false });
        expect(mentions(await s.diagnostics("main.cpp"), "missing")).toBe(true);
        expect(await s.pushed("main.cpp")).toBeUndefined();
    },
);

pulling({ "main.cpp": "int main() { return first; }\n" })("pull follows edits", async ({ s }) => {
    s.open("main.cpp", { pull: false });
    expect(mentions(await s.diagnostics("main.cpp"), "first")).toBe(true);

    const marker = requested(s);
    s.edit("main.cpp", { text: "int main() { return 0; }\n" });
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    s.edit("main.cpp", { text: "int main() { return second; }\n" });
    expect(mentions(await s.diagnostics("main.cpp"), "second")).toBe(true);
    // The client pulls after its own edits: no refresh is owed for them.
    expect(refreshes(s, marker)).toBe(0);
    expect(await s.pushed("main.cpp")).toBeUndefined();
});

pulling({ "main.cpp": "int a = first;\n" })("edit mid-pull answers the new text", async ({ s }) => {
    const hold = await s.hold("compile", "main.cpp");
    s.open("main.cpp", { pull: false });
    const pending = s.diagnostics("main.cpp");
    // The pull waits on its compile: the edit lands mid-parse.
    await hold.reached();
    s.edit("main.cpp", { text: "int a = second;\n" });
    await hold.release();

    const diagnostics = await pending;
    expect(mentions(diagnostics, "second")).toBe(true);
    expect(mentions(diagnostics, "first")).toBe(false);
});

pulling({ "main.cpp": "int main() { return 0; }\n" })(
    "cancelled pull answers at once",
    async ({ s }) => {
        const hold = await s.hold("compile", "main.cpp");
        s.open("main.cpp", { pull: false });
        const pull = s.send("textDocument/diagnostic", "main.cpp");
        await hold.reached();
        pull.cancel();
        // The next pull still waits on the compile the cancelled one left.
        let answered = false;
        const next = s.diagnostics("main.cpp").then((diagnostics) => {
            answered = true;
            return diagnostics;
        });
        await expect(pull.reply).rejects.toMatchObject({
            code: proto.LSPErrorCodes.RequestCancelled,
        });
        expect(answered).toBe(false);
        await hold.release();
        expect(await next).toEqual([]);
    },
);

pulling({
    "header.h": "inline int value() { return 1; }\n",
    "main.cpp": '#include "header.h"\nint main() { return value(); }\n',
})("recompile of same text refreshes", async ({ s }) => {
    s.open("main.cpp", { pull: false });
    expect(await s.diagnostics("main.cpp")).toEqual([]);

    s.disk.write("header.h", "inline int value() { return missing; }\n");
    const marker = requested(s);
    // Any request recompiles the document; its pulled answer went stale
    // with no edit to make the client pull again.
    await s.hover(at("main.cpp", "va|lue()"));
    await s.sync();
    expect(refreshes(s, marker), "diagnostic refresh after the header changed").toBeGreaterThan(0);
    expect(mentions(await s.diagnostics("main.cpp"), "missing")).toBe(true);
    expect(await s.pushed("main.cpp")).toBeUndefined();
});

// Fails before parsing on every attempt, and never settles.
pulling(
    { "main.cpp": "int main() { return 0; }\n" },
    { manifest: { units: { "main.cpp": ["--target=bogus-unknown-none"] } } },
)("repeated setup failure stays quiet", async ({ s }) => {
    s.open("main.cpp", { pull: false });
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    const marker = requested(s);
    await s.hover(at("main.cpp", "int |main"));
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    await s.sync();
    expect(refreshes(s, marker)).toBe(0);
});

pulling({ "main.cpp": "int main() { return missing; }\n" })(
    "closed document pulls empty",
    async ({ s }) => {
        s.open("main.cpp", { pull: false });
        expect(mentions(await s.diagnostics("main.cpp"), "missing")).toBe(true);
        s.close("main.cpp");
        expect(
            await s.request<proto.DocumentDiagnosticReport>("textDocument/diagnostic", "main.cpp"),
        ).toMatchObject({ kind: proto.DocumentDiagnosticReportKind.Full, items: [] });
        expect(await s.pushed("main.cpp")).toBeUndefined();
    },
);

pulling(
    { "real/main.cpp": "int main() { return missing; }\n" },
    {
        setup: (workspace) => {
            fs.symlinkSync(workspace.path("real"), workspace.path("link"));
        },
    },
).skipIf(process.platform === "win32")("second name pulls its own answer", async ({ s }) => {
    s.open("real/main.cpp", { pull: false });
    s.open("link/main.cpp", { pull: false });
    expect(mentions(await s.diagnostics("link/main.cpp"), "missing"), "equal texts share").toBe(
        true,
    );

    let marker = requested(s);
    s.edit("real/main.cpp", { text: "int main() { return 0; }\n" });
    await s.sync();
    expect(refreshes(s, marker), "refresh once the texts part").toBeGreaterThan(0);
    expect(messages(await s.diagnostics("link/main.cpp"))).toEqual([
        expect.stringContaining("also open as"),
    ]);

    marker = requested(s);
    s.close("real/main.cpp");
    await s.sync();
    expect(refreshes(s, marker), "refresh once the second name takes over").toBeGreaterThan(0);
    expect(mentions(await s.diagnostics("link/main.cpp"), "missing")).toBe(true);
    expect(await s.pushed("real/main.cpp")).toBeUndefined();
    expect(await s.pushed("link/main.cpp")).toBeUndefined();
});
