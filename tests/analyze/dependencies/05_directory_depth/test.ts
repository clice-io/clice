import { test } from "@clice/tools/testing";

// By default a file's module is its whole directory; --depth 1 names it
// by the first segment, src.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view edge --from src/util --to src/core");
    await t.cli("analyze modules --depth 1 --view module --module src");
});
