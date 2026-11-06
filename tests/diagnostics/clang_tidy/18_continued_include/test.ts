import { test } from "@clice/tools/testing";

// The preamble's include, continued onto the next line, reaches the
// checks: the one after the preamble repeats it.
test(async (t) => {
    // Unused includes come with the background index, before this pull or
    // after it.
    const s = await t.start({ config: { diagnostics: { unused_includes: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
