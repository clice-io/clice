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

// Without a cache store there is nowhere to put the file: the index is
// dropped with a warning on the file, and the AST still answers.
test("oversized index without store", { errors: ["Cannot create the log"] }, async (t) => {
    t.disk.write("big.cpp", BIG);
    t.disk.write(".clice", "not a directory\n");
    const s = await t.start({ env: LIMIT });
    await t.show("diagnostics", s.diagnostics("big.cpp"));
    await t.show("hover", s.hover(t.at("big.cpp", "int |function_000()")));
});

// With no PCH, an open file's index covers its headers too; too large for
// one message without a store, it covers the file alone, which keeps the
// file's own answers.
test("headers left out without store", { errors: ["Cannot create the log"] }, async (t) => {
    t.disk.write("big.h", generate.functions(200));
    t.disk.write(
        "main.cpp",
        '#include "big.h"\nint use() { return function_000(); }\nint again() { return use(); }\n',
    );
    t.disk.write(".clice", "not a directory\n");
    const s = await t.start({
        env: { CLICE_TEST_MAX_INDEX_BYTES: "4096" },
        lastRule: { background_index: false },
    });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("references", s.references(t.at("main.cpp", "int |use()")));
});
