import { expect } from "vitest";
import type * as proto from "vscode-languageserver-protocol";
import { test, type Serve } from "@clice/tools/testing";

/// The document version each action's edits are stamped with.
async function versions(s: Serve, line: number, character: number): Promise<(number | null)[]> {
    const position = { line, character };
    const actions = await s.request<proto.CodeAction[]>("textDocument/codeAction", "main.cpp", {
        range: { start: position, end: position },
        context: { diagnostics: [] },
    });
    return actions.flatMap((action) =>
        (action.edit?.documentChanges ?? []).map((change) =>
            "textDocument" in change ? change.textDocument.version : null,
        ),
    );
}

// An editor refuses an edit once its buffer moved past the version the edit
// names: every action carries the version it was computed for.
test(async (t) => {
    const s = await t.start({
        capabilities: { workspace: { workspaceEdit: { documentChanges: true } } },
    });
    await s.diagnostics("main.cpp");
    const { position } = s.position(t.marker("f"));
    expect(await versions(s, position.line, position.character)).toEqual([0, 0]);
    s.edit("main.cpp", { replace: "int x = 3", with: "int x = 4" });
    await s.diagnostics("main.cpp");
    expect(await versions(s, position.line, position.character)).toEqual([1, 1]);
    await t.show("f", s.codeActions(t.at("main.cpp", "int |f(int x = 4)")));
});
