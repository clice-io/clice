import { test } from "@clice/tools/testing";

// The scan resolved shared.h's include under each host's directories: only
// b.cpp enters config_b/config.h, though a.cpp would rank first.
test(async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("config_b/config.h"));
    await t.show("contexts", s.contexts("config_b/config.h"));
});
