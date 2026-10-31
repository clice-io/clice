import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test, type Serve } from "@clice/tools/testing";

// The folder change read with a request moves the document to the nested
// folder's project, which knows no command for it, before the request's work
// starts. Raw messages: their arrival in one read is the point.

/// Send `request` and the removal of the folder `outer` in one write; the
/// reply to the request.
async function readWithMove(
    s: Serve,
    request: { method: string; params: object },
): Promise<proto.ResponseMessage | undefined> {
    const replies = await s.client.sendTogether([
        { jsonrpc: "2.0", id: "asked", ...request },
        {
            jsonrpc: "2.0",
            method: proto.DidChangeWorkspaceFoldersNotification.method,
            params: { event: { added: [], removed: [{ uri: s.uri("outer"), name: "outer" }] } },
        },
    ]);
    return replies.get("asked");
}

// A pull never answers an error: it follows the document to its new project
// and answers that project's compile.
test("a pull follows the document", async (t) => {
    const s = await t.start({ folders: ["outer/inner", "outer"] });
    expect(await s.errors("outer/inner/main.cpp")).toEqual([]);
    const reply = await readWithMove(s, {
        method: proto.DocumentDiagnosticRequest.method,
        params: { textDocument: { uri: s.uri("outer/inner/main.cpp") } },
    });
    const report = reply?.result as proto.FullDocumentDiagnosticReport;
    await t.show(
        "pulled",
        report.items.map(({ message }) => (typeof message === "string" ? message : message.value)),
    );
});

// The folder removed and added again in the same read: the project taking
// the root over starts once the removed one is gone, which the pull, routed
// to the removed one, must not keep alive.
test("a pull follows into the project taking the root over", async (t) => {
    const s = await t.start({ folders: ["outer/inner", "outer"] });
    expect(await s.errors("outer/inner/main.cpp")).toEqual([]);
    const folder = { uri: s.uri("outer"), name: "outer" };
    const replies = await s.client.sendTogether([
        {
            jsonrpc: "2.0",
            id: "pull",
            method: proto.DocumentDiagnosticRequest.method,
            params: { textDocument: { uri: s.uri("outer/inner/main.cpp") } },
        },
        {
            jsonrpc: "2.0",
            method: proto.DidChangeWorkspaceFoldersNotification.method,
            params: { event: { added: [], removed: [folder] } },
        },
        {
            jsonrpc: "2.0",
            method: proto.DidChangeWorkspaceFoldersNotification.method,
            params: { event: { added: [folder], removed: [] } },
        },
    ]);
    const report = replies.get("pull")?.result as proto.FullDocumentDiagnosticReport;
    await t.show(
        "pulled",
        report.items.map(({ message }) => (typeof message === "string" ? message : message.value)),
    );
});

// Completion takes its ticket as its work starts, by then in a project the
// request was not routed to: it answers ContentModified.
test("a completion is outdated", async (t) => {
    const s = await t.start({ folders: ["outer/inner", "outer"] });
    expect(await s.errors("outer/inner/main.cpp")).toEqual([]);
    const reply = await readWithMove(s, {
        method: proto.CompletionRequest.method,
        params: {
            textDocument: { uri: s.uri("outer/inner/main.cpp") },
            position: { line: 3, character: 0 },
        },
    });
    await t.show("reply", { code: reply?.error?.code ?? 0, message: reply?.error?.message ?? "" });
});
