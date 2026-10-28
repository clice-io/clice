import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The outline the index serves an unedited file tags a deprecated symbol as
// the compiled file's does.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("outline", s.documentSymbols("main.cpp"), { parts: ["tags"] });
    expect((await s.counts()).compile).toBe(0);
});
