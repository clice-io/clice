import { test } from "@clice/tools/testing";

// A fatal error before the include point silences every diagnostic after
// it; it shows at the top of the fragment instead of leaving it spotless.
test(async (t) => {
    const s = await t.start();
    await t.show("ops.def", s.diagnostics("ops.def"));
});
