import { test } from "@clice/tools/testing";

// A header the includer misses before the include point shows at the top of
// the fragment: what goes wrong in the fragment likely stems from it.
test(async (t) => {
    const s = await t.start();
    await t.show("ops.def", s.diagnostics("ops.def"));
});
