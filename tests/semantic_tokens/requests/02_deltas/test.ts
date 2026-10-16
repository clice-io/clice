import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const FULL = "textDocument/semanticTokens/full";
const DELTA = "textDocument/semanticTokens/full/delta";

// After an edit the server sends what changed since the answer the client
// holds: that answer patched with the edits is the whole new answer.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const before = await s.request<proto.SemanticTokens>(FULL, "main.cpp");
    s.edit("main.cpp", { replace: "int third = second + 1;", with: "int third = 3;" });
    await s.diagnostics("main.cpp");
    const delta = await s.request<proto.SemanticTokensDelta>(DELTA, "main.cpp", {
        previousResultId: before.resultId,
    });
    const after = await s.request<proto.SemanticTokens>(FULL, "main.cpp");
    const patched = [...before.data];
    for (const edit of [...delta.edits].reverse()) {
        patched.splice(edit.start, edit.deleteCount, ...(edit.data ?? []));
    }
    expect(patched).toEqual(after.data);
    await t.show(
        "edits",
        delta.edits.map(
            (edit) =>
                `at ${edit.start}: ${edit.deleteCount} removed, ${edit.data?.length ?? 0} added`,
        ),
    );

    // An answer the server no longer holds gets the whole new answer.
    const unknown = await s.request<proto.SemanticTokens>(DELTA, "main.cpp", {
        previousResultId: "unknown",
    });
    expect(unknown.data).toEqual(after.data);
});
