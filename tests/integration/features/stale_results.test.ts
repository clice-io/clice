/// A request whose buffer moved on mid-flight answers ContentModified, never
/// a result computed on the old text and never null.
///
/// Why an error and not null: to a client, null is a real answer — "this
/// document has nothing". VS Code's semantic-token pipeline keeps a full
/// request in flight across keystrokes (it does not cancel on edit; it
/// reconciles the reply with the edits made meanwhile), and on a null reply
/// it clears every semantic token of the document, so the whole file falls
/// back to TextMate colors until the next pull lands — the flicker users saw
/// while typing. On a ContentModified error the same pipeline keeps the
/// tokens it has and schedules a re-pull; the client even advertises this in
/// `staleRequestSupport.retryOnContentModified`. Inlay hints, folds and the
/// outline behave the same way: an empty reply is applied, an error is not.
///
/// Why not the old result either: whole-document replies carry positions of
/// the text they were computed on; the client would map them onto the
/// edited buffer at the wrong places (formatting edits would even corrupt
/// the file). The server has no AST for the old buffer anymore once the edit
/// superseded the compile, so the only honest answer is "changed, ask again".
///
/// Completion is the exception while the edits sit at or past its cursor:
/// VS Code neither cancels nor re-asks a completion the user keeps typing
/// into — it filters the reply by what was typed meanwhile, and treats
/// ContentModified as an empty list. An edit before the cursor moves the
/// reply's ranges, so that one still answers ContentModified.

import * as proto from "vscode-languageserver-protocol";
import { SLOW_SOURCE as SLOW, sleep } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

// Completion skips most of the work a full build does and can finish the
// body within EDIT_SUPERSEDE_DELAY on a fast machine.
const COMPLETION_EDIT_DELAY = 30;

test("edit mid-flight still completes", async ({ session }) => {
    const { client, workspace } = session.tmp();
    const body = SLOW + "int extra_value;\nint probe = extra_";
    workspace.write("slow.cpp", body);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("slow.cpp");
    const line = body.split("\n").length - 1;
    const pending = client.completionAt(uri, line, "int probe = extra_".length);
    await sleep(COMPLETION_EDIT_DELAY);
    client.change(uri, 1, body + "v");

    const reply = await pending;
    const items = Array.isArray(reply) ? reply : (reply?.items ?? []);
    expect(items.map((item) => item.label)).toContain("extra_value");

    const moved = client.completionAt(uri, line, "int probe = extra_".length);
    await sleep(COMPLETION_EDIT_DELAY);
    client.change(uri, 2, "int moved;\n" + body);
    await expect(moved).rejects.toMatchObject({ code: proto.LSPErrorCodes.ContentModified });
}, 300_000);

// The messages below are written in one write, which the server reads
// together: a request still belongs to the text it was asked about, though
// the edit read with it is applied before its task starts.

function request(id: string, method: string, params: object): proto.RequestMessage {
    return { jsonrpc: "2.0", id, method, params };
}

function notification(method: string, params: object): proto.NotificationMessage {
    return { jsonrpc: "2.0", method, params };
}

function edit(uri: string, text: string): proto.NotificationMessage {
    return notification(proto.DidChangeTextDocumentNotification.method, {
        textDocument: { uri, version: 2 },
        contentChanges: [{ text }],
    });
}

for (const [method, params] of [
    ["textDocument/hover", { position: { line: 0, character: 4 } }],
    ["textDocument/formatting", { options: { tabSize: 4, insertSpaces: true } }],
] as const) {
    test(`${method} read with an edit answers ContentModified`, async ({ session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("main.cpp", "int value = 1;\n");
        workspace.writeCDB(["main.cpp"]);
        await client.initialize(workspace);
        const [uri] = await client.openAndWait("main.cpp");

        const replies = await client.sendTogether([
            request(method, method, { textDocument: { uri }, ...params }),
            edit(uri, "int  value = 2;\n"),
        ]);
        expect(replies.get(method)?.error?.code).toBe(proto.LSPErrorCodes.ContentModified);
    }, 120_000);
}

const COMPLETING = "int extra_value;\nint probe = extra_";

test("completion read with an edit is served", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", COMPLETING);
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const replies = await client.sendTogether([
        request("completion", "textDocument/completion", {
            textDocument: { uri },
            position: { line: 1, character: 18 },
        }),
        edit(uri, COMPLETING + "v"),
    ]);
    const reply = replies.get("completion")?.result as
        | proto.CompletionList
        | proto.CompletionItem[];
    const items = Array.isArray(reply) ? reply : reply.items;
    expect(items.map((item) => item.label)).toContain("extra_value");
}, 120_000);

test("completion read with a reopen answers ContentModified", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", COMPLETING);
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const replies = await client.sendTogether([
        request("completion", "textDocument/completion", {
            textDocument: { uri },
            position: { line: 1, character: 18 },
        }),
        notification(proto.DidCloseTextDocumentNotification.method, { textDocument: { uri } }),
        notification(proto.DidOpenTextDocumentNotification.method, {
            textDocument: { uri, languageId: "cpp", version: 1, text: COMPLETING },
        }),
    ]);
    expect(replies.get("completion")?.error?.code).toBe(proto.LSPErrorCodes.ContentModified);
}, 120_000);
