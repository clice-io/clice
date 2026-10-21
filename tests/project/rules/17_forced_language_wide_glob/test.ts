import { test } from "@clice/tools/testing";

// The rule forces C++ for every file under src/, which holds a build
// script and a readme beside the one source.
test(async (t) => {
    await t.cli("index");
});
