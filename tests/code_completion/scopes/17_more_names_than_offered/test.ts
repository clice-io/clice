import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The index offers a compile at most a hundred of a scope's names, which
// stand in for the preamble's: the list says there are more, so typing more
// asks again, in C, whose names live on its identifiers, as in C++. No item
// limit cuts it here.
test("more names than the index offers", async (t) => {
    const s = await t.start({
        config: { code_completion: { limit: 0 }, project: { enable_indexing: false } },
    });
    for (const name of ["cpp", "c"]) {
        const marker = t.marker(name);
        await s.diagnostics(marker.path);
        const list = await s.completion(marker);
        expect(list.items.length).toBeLessThan(120);
        expect(list.incomplete).toBe(true);
    }
});
