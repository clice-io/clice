import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A compile reads at most a hundred of the names its preamble declares that
// the typed one matches: the list says there are more, so typing more asks
// again, in C, whose names live on its identifiers, as in C++. Neither the
// item limit nor the index takes part here.
test("more names than a compile reads", async (t) => {
    const s = await t.start({
        config: { code_completion: { limit: 0 }, project: { enable_indexing: false } },
    });
    for (const name of ["cpp", "c"]) {
        const marker = t.marker(name);
        await s.diagnostics(marker.path);
        const list = await s.completion(marker);
        expect(list.items.length).toBe(100);
        expect(list.incomplete).toBe(true);
    }
});
