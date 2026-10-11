import { test } from "@clice/tools/testing";

// The include in the inactive branch is not where main.cpp enters utils.h.
test(async (t) => {
    const s = await t.start();
    await t.show("utils.h", s.diagnostics("utils.h"));
});
