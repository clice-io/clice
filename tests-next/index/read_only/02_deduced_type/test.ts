import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The index records what `auto` stands for: a read-only file navigates
// from it without compiling.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    const m = t.marker("auto");
    await t.show("auto", {
        definition: s.definition(m),
        type_definition: s.typeDefinition(m),
        hover: s.hover(m),
    });
    expect((await s.counts()).pch).toBe(0);
});
