import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query --method symbolSearch --symbol circ --limit 5");
});
