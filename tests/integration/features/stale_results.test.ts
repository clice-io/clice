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
import { sleep } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

// Two hundred thousand trivial declarations: slow to parse on any hardware,
// so an edit reliably lands while the request still waits on the compile.
const SLOW = Array.from({ length: 200_000 }, (_, i) => `int v${i};`).join("\n") + "\n";
const EDIT_SUPERSEDE_DELAY = 300;

test("edit mid-flight answers ContentModified", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("slow.cpp", SLOW);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("slow.cpp");
    const td: proto.TextDocumentIdentifier = { uri };
    const head: proto.Range = {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    };

    // Every AST-backed feature, each preceded by an edit so it launches a
    // fresh parse of the whole body: the second edit then lands while that
    // parse is still running — the "keep typing" case.
    const pulling: [string, unknown][] = [
        ["textDocument/hover", { textDocument: td, position: { line: 0, character: 4 } }],
        ["textDocument/semanticTokens/full", { textDocument: td }],
        ["textDocument/inlayHint", { textDocument: td, range: head }],
        ["textDocument/foldingRange", { textDocument: td }],
        ["textDocument/documentSymbol", { textDocument: td }],
        ["textDocument/documentLink", { textDocument: td }],
        ["textDocument/definition", { textDocument: td, position: { line: 0, character: 4 } }],
    ];
    let version = 0;
    for (const [method, params] of pulling) {
        version += 1;
        client.change(uri, version, SLOW + `int extra${version};\n`);
        const pending = client.sendRequest(method, params);
        await sleep(EDIT_SUPERSEDE_DELAY);
        version += 1;
        client.change(uri, version, SLOW + `int extra${version};\n`);
        await expect(pending, method).rejects.toMatchObject({
            code: proto.LSPErrorCodes.ContentModified,
        });
    }

    // The buffer settled: the client's re-pull waits for the fresh compile
    // and gets the real answer for the current text — the tokens that the
    // error told it to keep showing meanwhile are replaced, not blanked.
    const tokens = await client.semanticTokensFull(uri);
    expect(tokens).not.toBeNull();
    expect(tokens!.data.length).toBeGreaterThan(0);
    const hover = await client.hoverAt(uri, 0, 4);
    expect(hover).not.toBeNull();
}, 300_000);

test("edit mid-flight still completes", async ({ session }) => {
    const { client, workspace } = session.tmp();
    const body = SLOW + "int extra_value;\nint probe = extra_";
    workspace.write("slow.cpp", body);
    workspace.writeCDB(["slow.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("slow.cpp");
    const line = body.split("\n").length - 1;
    const pending = client.completionAt(uri, line, "int probe = extra_".length);
    await sleep(EDIT_SUPERSEDE_DELAY);
    client.change(uri, 1, body + "v");

    const reply = await pending;
    const items = Array.isArray(reply) ? reply : (reply?.items ?? []);
    expect(items.map((item) => item.label)).toContain("extra_value");

    const moved = client.completionAt(uri, line, "int probe = extra_".length);
    await sleep(EDIT_SUPERSEDE_DELAY);
    client.change(uri, 2, "int moved;\n" + body);
    await expect(moved).rejects.toMatchObject({ code: proto.LSPErrorCodes.ContentModified });
}, 300_000);
