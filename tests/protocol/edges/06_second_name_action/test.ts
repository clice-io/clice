import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The document naming an open file through a symlink shares the first
// document's answers while their texts agree; an action asked in it edits
// the document it was asked in. The rendering resolves the link, so the
// URIs are checked as the server sent them.

test("action from the second name", async (t) => {
    t.disk.write("real/main.cpp", t.disk.read("main.cpp"));
    t.disk.rm("main.cpp");
    const s = await t.start();
    t.disk.symlink("link", "real");
    await s.diagnostics("real/main.cpp");
    await s.diagnostics("link/main.cpp");
    const { uri, position } = s.position(t.at("link/main.cpp", "|auto tonnes"));
    const actions = await s.client.codeActions(uri, { start: position, end: position });
    const edited = (actions ?? []).flatMap((action) => {
        if (!("edit" in action)) {
            return [];
        }
        const { changes, documentChanges } = action.edit;
        return [
            ...Object.keys(changes ?? {}),
            ...(documentChanges ?? []).flatMap((change) =>
                "textDocument" in change ? [change.textDocument.uri] : [],
            ),
        ];
    });
    expect(edited.map((target) => s.spelling(target))).toEqual(["link/main.cpp"]);
    await t.show("actions", s.codeActions(t.at("link/main.cpp", "|auto tonnes")));
});
