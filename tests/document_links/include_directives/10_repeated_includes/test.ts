import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// Every include links, the second of a header #pragma once or its guard
// skips as well as the first.
answers("compiled", (a) => a.show("main.cpp", a.documentLinks("main.cpp")));

// The index answers the same before the file is compiled.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cpp", { pull: false });
    await s.indexed();
    await t.show("main.cpp", s.documentLinks("main.cpp"));
    expect((await s.counts()).compile).toBe(0);
});
