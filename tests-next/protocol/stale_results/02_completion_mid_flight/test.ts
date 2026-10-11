import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// Completion is the exception to stale results while the edits sit at or
// past its cursor: an editor neither cancels nor asks again a completion
// the user keeps typing into, it filters the reply by what was typed. An
// edit before the cursor moves the reply's ranges, and answers
// ContentModified.

const COMPLETING = "int extra_value;\nint probe = extra_";

function failure(error: unknown): { code: number; message: string } {
    expect(error).toBeInstanceOf(proto.ResponseError);
    const { code, message } = error as proto.ResponseError<unknown>;
    return { code, message };
}

test("an edit at the cursor", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const park = await s.park("completion", "main.cpp", { at: "worker" });
    const completion = s.completion(t.marker("complete"));
    await park.reached();
    s.edit("main.cpp", { after: "probe = extra_", insert: "v" });
    await park.release();
    await t.show("completion", completion);
});

test("an edit before the cursor", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const park = await s.park("completion", "main.cpp", { at: "worker" });
    const completion = s.completion(t.marker("complete")).then(
        () => null,
        (error: unknown) => error,
    );
    await park.reached();
    s.edit("main.cpp", { before: "int extra_value;", insert: "int moved;\n" });
    await park.release();
    await t.show("completion", failure(await completion));
});

// Messages written in one write are read together: a request still belongs
// to the text it was asked about, though the change read with it applies
// before its work starts.

test("read with an edit", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const uri = s.uri("main.cpp");
    // Raw messages: their arrival in one read is the point.
    const replies = await s.client.sendTogether([
        {
            jsonrpc: "2.0",
            id: "completion",
            method: "textDocument/completion",
            params: { textDocument: { uri }, position: { line: 1, character: 18 } },
        },
        {
            jsonrpc: "2.0",
            method: "textDocument/didChange",
            params: {
                textDocument: { uri, version: 2 },
                contentChanges: [{ text: `${COMPLETING}v` }],
            },
        },
    ]);
    const reply = replies.get("completion")?.result as
        proto.CompletionList | proto.CompletionItem[] | null;
    const items = Array.isArray(reply) ? reply : (reply?.items ?? []);
    await t.show(
        "completion",
        items.map((item) => item.label),
    );
});

test("read with a reopen", async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const uri = s.uri("main.cpp");
    const replies = await s.client.sendTogether([
        {
            jsonrpc: "2.0",
            id: "completion",
            method: "textDocument/completion",
            params: { textDocument: { uri }, position: { line: 1, character: 18 } },
        },
        {
            jsonrpc: "2.0",
            method: "textDocument/didClose",
            params: { textDocument: { uri } },
        },
        {
            jsonrpc: "2.0",
            method: "textDocument/didOpen",
            params: { textDocument: { uri, languageId: "cpp", version: 1, text: COMPLETING } },
        },
    ]);
    const error = replies.get("completion")?.error;
    await t.show("completion", { code: error?.code ?? 0, message: error?.message ?? "" });
});
