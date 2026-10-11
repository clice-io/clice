import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --view edge --from util --to core");
});
