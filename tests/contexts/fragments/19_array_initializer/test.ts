import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("cpu/microcode/defs.h"));
});
