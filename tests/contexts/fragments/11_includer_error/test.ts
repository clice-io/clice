import { test } from "@clice/tools/testing";

// An error in the includer's code past the include point lands where the
// fragment takes part in it.
test(async (t) => {
    const s = await t.start();
    await t.show("m.inc", s.diagnostics("m.inc"));
});
