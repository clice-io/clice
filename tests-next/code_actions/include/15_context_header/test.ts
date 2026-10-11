import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// utils.h is no unit of its own: it compiles in main.cpp's context, which
// the server synthesizes. The directive goes inside the header, after its
// #pragma once, and the header then compiles in that context.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: true } } });
    await s.diagnostics("main.cpp");
    await s.indexed();
    await s.diagnostics("utils.h");
    expect((await s.stats()).synthesizedContexts).toBe(1);
    await t.show("helper", s.codeActions(t.marker("helper"), { compile: true }));
});
