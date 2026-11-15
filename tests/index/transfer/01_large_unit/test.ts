import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// An index too large for one message between clice processes travels
// through a file in the cache instead. CLICE_TEST_MAX_INDEX_BYTES lowers
// that limit so even this file's index takes the route.
const LIMIT = { CLICE_TEST_MAX_INDEX_BYTES: "1024" };
const BIG = `${generate.functions(200)}int use() { return function_000(); }\n`;

test("open file index via file", async (t) => {
    t.disk.write("big.cpp", BIG);
    const s = await t.start({ env: LIMIT, lastRule: { background_index: false } });
    await t.show("diagnostics", s.diagnostics("big.cpp"));
    await t.show("references", s.references(t.at("big.cpp", "int |function_000()")));
    await s.sync();
    expect((await s.stats()).pendingTmpFiles).toBe(0);
});
