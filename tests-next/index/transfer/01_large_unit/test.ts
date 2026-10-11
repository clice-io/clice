import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// An index too large for one message between clice processes travels
// through a file in the cache instead. CLICE_TEST_MAX_INDEX_BYTES lowers
// that limit so even this file's index takes the route.
const LIMIT = { CLICE_TEST_MAX_INDEX_BYTES: "1024" };
const BIG = `${generate.functions(200)}int use() { return function_000(); }\n`;

test("open file index via file", async (t) => {
    t.disk.write("big.cpp", BIG);
    const s = await t.start({ env: LIMIT, config: { project: { enable_indexing: false } } });
    await t.show("diagnostics", s.diagnostics("big.cpp"));
    await t.show("references", s.references(t.at("big.cpp", "int |function_000()")));
    await s.sync();
    expect((await s.stats()).pendingTmpFiles).toBe(0);
});

// Without a cache store there is nowhere to put the file: the index is
// dropped with a warning on the file, and the AST still answers.
test("oversized index without store", async (t) => {
    t.disk.write("big.cpp", BIG);
    t.disk.write(".clice", "not a directory\n");
    const s = await t.start({ env: LIMIT });
    await t.show("diagnostics", s.diagnostics("big.cpp"));
    await t.show("hover", s.hover(t.at("big.cpp", "int |function_000()")));
});
