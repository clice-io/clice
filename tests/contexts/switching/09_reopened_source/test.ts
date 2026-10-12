import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// After a switch an editor closes and reopens the document; the reopened
// file compiles under the command chosen.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    const listed = await s.contexts("main.cpp");
    const hash = (flag: string) => listed.contexts.find((c) => c.label.includes(flag))?.commandHash;
    const [a, b] = [hash("USE_A"), hash("USE_B")];
    expect(
        (
            await s.switchContext("main.cpp", "main.cpp", {
                commandHash: a ?? "",
                epoch: listed.epoch,
            })
        ).success,
    ).toBe(true);
    await t.show("with USE_A", s.diagnostics("main.cpp"));
    expect((await s.switchContext("main.cpp", "main.cpp", { commandHash: b ?? "" })).success).toBe(
        true,
    );
    s.close("main.cpp");
    s.open("main.cpp");
    await t.show("reopened", s.diagnostics("main.cpp"));
    expect((await s.currentContext("main.cpp")).context?.commandHash).toBe(b);
});
