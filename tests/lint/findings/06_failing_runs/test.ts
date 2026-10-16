import { test } from "@clice/tools/testing";

// A finding fails the run; the same project without one passes.

test(async (t) => {
    await t.cli("lint --jobs 2");
    t.disk.write("main.cpp", "int add(int a, int b) { return a + b; }\n");
    await t.cli("lint --jobs 2");
});
