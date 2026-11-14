import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Before the file compiles, the index links the lines naming a file the
// parse does not enter, as the AST does once it is current.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cpp", { pull: false });
    await s.indexed();
    await t.show("main.cpp", s.documentLinks("main.cpp"));
    expect((await s.counts()).compile).toBe(0);
});
