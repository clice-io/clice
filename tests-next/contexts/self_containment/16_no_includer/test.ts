import { test } from "@clice/tools/testing";

// No source includes the header, and it does not compile on its own.
test(async (t) => {
    const s = await t.start();
    await t.show("orphan.h", s.diagnostics("orphan.h"));
    await t.show("contexts", s.contexts("orphan.h"));
});
