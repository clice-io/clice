import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("format scratch.cpp");
    await t.cli("format src");
    await t.cli("format vendor");
});
