import { expect } from "vitest";
import type * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A client that does not announce versioned document changes gets plain
// changes keyed by document.
test(async (t) => {
    const s = await t.start({ capabilities: {} });
    await s.diagnostics("main.cpp");
    const { position } = s.position(t.marker("f"));
    const actions = await s.request<proto.CodeAction[]>("textDocument/codeAction", "main.cpp", {
        range: { start: position, end: position },
        context: { diagnostics: [] },
    });
    expect(actions.map((action) => action.edit?.documentChanges)).toEqual([undefined, undefined]);
    expect(actions.map((action) => Object.keys(action.edit?.changes ?? {}))).toEqual([
        [s.uri("main.cpp")],
        [s.uri("main.cpp")],
    ]);
    await t.show("f", s.codeActions(t.marker("f")));
});
