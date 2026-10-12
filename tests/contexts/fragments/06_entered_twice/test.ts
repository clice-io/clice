import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The header's enum enters the list first; the source's table enters it
// again after including the header, where the entries are strings.
test(async (t) => {
    const s = await t.start();
    await t.show("in the enum", {
        diagnostics: s.diagnostics("ops.def"),
        symbols: s.documentSymbols("ops.def"),
    });
    await s.sync();
    const park = await s.park("compile", "ops.def", { at: "worker" });
    expect((await s.switchContext("ops.def", "ops.cpp", { occurrence: 1 })).success).toBe(true);
    await park.reached();
    const pending = s.documentSymbols("ops.def");
    // A later reply proves the server took the request up first.
    await s.counts();
    await park.release();
    await t.show("while the switch compiles", pending);
    await s.sync();
    await t.show("in the table", {
        diagnostics: s.diagnostics("ops.def"),
        symbols: s.documentSymbols("ops.def"),
    });
});
