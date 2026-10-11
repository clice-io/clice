import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The default between the two commands is the content's choice, not the
// database's order: the test picks each explicitly.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    const listed = await s.contexts("main.cpp");
    await t.show("contexts", s.contexts("main.cpp"));
    const defined = listed.contexts.find((c) => c.label.includes("-DEXPECTED"))?.commandHash;
    const plain = listed.contexts.find((c) => !c.label.includes("-DEXPECTED"))?.commandHash;
    expect([defined, plain].every((hash) => hash !== undefined && hash !== "")).toBe(true);
    const pick = async (hash: string | undefined, label: string) => {
        const switched = await s.switchContext("main.cpp", "main.cpp", { commandHash: hash ?? "" });
        expect(switched.success).toBe(true);
        expect((await s.currentContext("main.cpp")).context?.commandHash).toBe(hash);
        await t.show(label, s.diagnostics("main.cpp"));
    };
    await pick(defined, "with the define");
    await pick(plain, "without it");
    await pick(defined, "with it again");
});
