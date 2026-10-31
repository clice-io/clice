import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The document naming an open file through a symlink shares the first
// document's answers while their texts agree; an action asked in it edits
// the document it was asked in. The rendering resolves the link, so the
// URIs are checked as the server sent them.

const POSIX = { except: { win32: "symlinks need privileges on Windows" } };

test("action from the second name", POSIX, async (t) => {
    t.disk.write("real/main.cpp", t.disk.read("main.cpp"));
    t.disk.rm("main.cpp");
    const s = await t.start();
    t.disk.symlink("link", "real");
    await s.diagnostics("real/main.cpp");
    await s.diagnostics("link/main.cpp");
    const at = { line: 1, character: 4 };
    const actions = await s.client.codeActions(s.uri("link/main.cpp"), { start: at, end: at });
    const edited = (actions ?? []).flatMap((action) => {
        if (!("edit" in action) || action.edit === undefined) {
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
    expect(edited.length).toBeGreaterThan(0);
    for (const uri of edited) {
        expect(decodeURIComponent(new URL(uri).pathname)).toMatch(/\/link\/main\.cpp$/);
    }
    await t.show("actions", s.codeActions(t.at("link/main.cpp", "|auto tonnes")));
});
