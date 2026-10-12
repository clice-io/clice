import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// Fifty edits in a row, each followed by a hover nobody waits for, as an
// editor sends them while the user types: every hover is answered, or
// told its text moved on, and the last one answers on the last text.

test(async (t) => {
    const s = await t.start();
    const add = t.at("main.cpp", "int |add");
    await s.diagnostics("main.cpp");
    const hovers: Promise<unknown>[] = [];
    for (let edit = 0; edit < 50; edit++) {
        s.edit("main.cpp", {
            text: t.disk.read("main.cpp").replace("a + b;", `a + b + ${String(edit)};`),
        });
        hovers.push(
            s.hover(add).then(
                () => null,
                (error: unknown) => error,
            ),
        );
    }
    for (const outcome of await Promise.all(hovers)) {
        if (outcome !== null) {
            expect(outcome).toMatchObject({ code: proto.LSPErrorCodes.ContentModified });
        }
    }
    await t.show("hover", s.hover(add));
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});
