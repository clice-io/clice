import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules edge util core");
    await t.cli("analyze modules file util/total.h");
    await t.cli("analyze modules file util/made.h");
    await t.cli("analyze modules file util/converted.h");
});
