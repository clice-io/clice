import { test } from "@clice/tools/testing";

// By default a file's module is its whole directory; --dir-depth 1 names it
// by the first segment, src.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules edge src/util src/core");
    await t.cli("analyze modules module src --dir-depth 1");
});
