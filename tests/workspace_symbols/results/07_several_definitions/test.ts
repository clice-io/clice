import { test } from "@clice/tools/testing";

// A C function a weak and a strong definition give is one symbol two
// sources define: the search lists it at each.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("father", s.workspaceSymbols("father"));
});
