import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// The background index of a source too large for one message between
// clice processes reaches the master through a file in the cache, which is
// removed once read. CLICE_TEST_MAX_INDEX_BYTES lowers that limit.
test(async (t) => {
    t.disk.write(
        "b.cpp",
        `#include "h.h"\nint foo(int a) { return a; }\n${generate.functions(200)}`,
    );
    const s = await t.start({ env: { CLICE_TEST_MAX_INDEX_BYTES: "1024" } });
    s.open("main.cpp");
    await s.indexed();
    await t.show("definition", s.definition(t.at("main.cpp", "return |foo(1)")));
    expect((await s.stats()).pendingTmpFiles).toBe(0);
});
