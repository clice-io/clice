import { generate, test } from "@clice/tools/testing";

// `clice index` takes the same route for a unit whose index is too large
// for one message (CLICE_TEST_MAX_INDEX_BYTES lowers the limit): every
// reference still lands in the persisted index.
test(async (t) => {
    t.disk.write("a.cpp", `int alpha() { return 1; }\n${generate.functions(200)}`);
    const env = { CLICE_TEST_MAX_INDEX_BYTES: "1024" };
    await t.cli("index", { env });
    await t.cli("index --show-symbol alpha", { env });
});
