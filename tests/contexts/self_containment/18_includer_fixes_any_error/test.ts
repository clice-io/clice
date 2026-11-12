import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Each header compiles alone with errors no list of missing names knows:
// rows of the includer's array, labels of the includer's function, a
// library function the includer declares. Each has fewer in the includer's
// context, which it compiles in.
test(async (t) => {
    const s = await t.start();
    const headers = ["rows.h", "targets.h", "smooth.h"];
    for (const header of headers) {
        await t.show(header, s.diagnostics(header));
    }
    expect((await s.stats()).synthesizedContexts).toBe(headers.length);
});
