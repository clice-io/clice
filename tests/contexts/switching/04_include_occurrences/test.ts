import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A header without a guard, included twice by one host, has a context per
// include.
test("each include is a context", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("list.def")).toEqual([]);
    await t.show("contexts", s.contexts("list.def"));
    const occurrences = (await s.contexts("list.def")).contexts.map((c) => c.occurrence);
    expect(occurrences.sort()).toEqual([0, 1]);
    for (const occurrence of [0, 1]) {
        const switched = await s.switchContext("list.def", "main.cpp", { occurrence });
        expect(switched.success).toBe(true);
        await s.sync();
        const current = await s.currentContext("list.def");
        expect([current.context?.occurrence, current.automatic]).toEqual([occurrence, false]);
        await t.show(`include ${String(occurrence + 1)}`, {
            diagnostics: s.diagnostics("list.def"),
            symbols: s.documentSymbols("list.def"),
        });
    }
});

test("an include past the last is refused", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("list.def");
    const switched = await s.switchContext("list.def", "main.cpp", { occurrence: 5 });
    expect(switched.success).toBe(false);
    await t.show("current", s.currentContext("list.def"));
});

test("inspect takes an include", async (t) => {
    // `--host` and `--occurrence` choose the context as switchContext does.
    await t.cli("inspect document-symbols list.def --host main.cpp --occurrence 0");
    await t.cli("inspect document-symbols list.def --host main.cpp --occurrence 1");
});
