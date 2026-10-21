import { test } from "@clice/tools/testing";

// main.cpp imports m, and its unit records main.cpp's own declarations only,
// also after lint --index, whose clang-tidy pass over the whole unit loads
// the imported module's declarations first.
test("index", async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("index unit main.cpp");
});

test("lint --index", async (t) => {
    await t.cli("lint --index --jobs 2");
    await t.cli("index unit main.cpp");
});
