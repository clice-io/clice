import { test } from "@clice/tools/testing";

// The editor reports the literals `clice lint` reports.
test(async (t) => {
    await t.cli("lint --jobs 2");
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
