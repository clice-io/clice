import { test } from "@clice/tools/testing";

// The includer imports a module nothing provides before it enters the
// fragment: the fatal error shows at the fragment's top, and no guidance
// blames the fragment's command, which could not provide the module.
test(async (t) => {
    const s = await t.start();
    await t.show("ops.def", s.diagnostics("ops.def"));
});
