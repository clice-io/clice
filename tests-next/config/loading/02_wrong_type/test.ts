import { test } from "@clice/tools/testing";

const INVALID = { errors: ["Invalid clice\\.toml .*invalid type"] };

test("a wrong type is an error", INVALID, async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});

// The next server publishes an empty list for the fixed file, which clears
// what an editor still shows from the last session.
test("the fix clears it", INVALID, async (t) => {
    const s = await t.start();
    await t.show("broken", s.pushes("clice.toml"));
    await s.offline(() => {
        t.disk.write("clice.toml", "[project]\nenable_indexing = true\n");
    });
    await t.show("fixed", s.pushes("clice.toml"));
});
