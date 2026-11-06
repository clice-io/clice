import { test } from "@clice/tools/testing";

// Only a check about something deprecated strikes the code through; one
// suggesting a modern spelling leaves it as it is.
test(async (t) => {
    // The unused `<stdlib.h>` is reported where a guard marks it, with the
    // background index, before this pull or after it.
    const s = await t.start({ config: { diagnostics: { unused_includes: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
});
