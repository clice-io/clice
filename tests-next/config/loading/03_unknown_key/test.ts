import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A misspelled key warns, and the rest of the file still applies: nothing is
// indexed.
test(async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    expect((await s.counts()).index).toBe(0);
});
