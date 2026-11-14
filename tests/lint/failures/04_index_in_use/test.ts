import { test } from "@clice/tools/testing";

// Pins lint beside an editor's server on the same workspace: a plain run
// reads the index the server owns and checks all the same, while --index,
// which must write that index, cannot.
test("beside a server", { errors: ["holds the index writer lock"] }, async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.cli("lint --jobs 2");
    await t.cli("lint --index --jobs 2", { env: { CLICE_TEST_WRITER_WAIT_MS: "100" } });
});
