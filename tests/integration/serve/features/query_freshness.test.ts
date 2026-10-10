/// Navigation right after an edit must resolve against the edited buffer:
/// the server settles the file's compile before answering, with no timeout.

import { at, expect, serve } from "../../fixtures.ts";

serve("tiny")("navigation after change", async ({ s }) => {
    await s.compiled("main.cpp");

    // Inserts a line at the top: every position shifts, so a query
    // resolved against the pre-edit index would name the wrong symbol or
    // nothing. No wait after the edit: the requests below must settle the
    // compile themselves before resolving the cursor.
    s.edit("main.cpp", { before: "int add(", insert: "// shift\n" });
    const call = at("main.cpp", "add(1, 2)");

    expect(s.show(await s.definition(call))).toBe("main.cpp: int add(int lhs, int rhs) {");
    expect(
        s.show(
            await s.request("textDocument/references", call, {
                context: { includeDeclaration: false },
            }),
        ),
    ).toBe("main.cpp: int value = add(1, 2);");
});
