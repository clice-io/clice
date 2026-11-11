import { test } from "@clice/tools/testing";

// Fw.h needs main.cpp's FW_SETTING and is reached only as <Fw/Fw.h> through
// main.cpp's -F: with no index to ask, the scan finds it in
// Fw.framework/Headers and names main.cpp its host.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const header = "frameworks/Fw.framework/Headers/Fw.h";
    await t.show("diagnostics", s.diagnostics(header));
    await t.show("contexts", s.contexts(header));
});
