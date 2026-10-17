import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query definition checksum");
});
