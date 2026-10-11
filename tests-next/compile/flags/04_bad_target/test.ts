import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// clang cannot start on this command: the compile fails before parsing.

test("unrunnable command stays dirty", async (t) => {
    const s = await t.start();
    await t.show("first", s.diagnostics("main.cpp"));
    // No product was made: the next request compiles again instead of
    // serving one.
    await s.hover(t.at("main.cpp", "|main"));
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.compile).toBe(2);
    await t.show("retried", s.diagnostics("main.cpp"));
});
