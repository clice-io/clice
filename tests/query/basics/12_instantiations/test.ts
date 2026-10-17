import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query references Box::open");
    await t.cli("query call-graph Box::open --direction callers");
    await t.cli("query references reset");
});
