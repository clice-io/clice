import { test } from "@clice/tools/testing";

// A class template and its explicit specialization: the name alone matches
// both, the specialization is named with its arguments.
test(async (t) => {
    await t.cli("index");
    await t.cli("query --method references --symbol Holder");
    await t.cli(["query", "--method", "references", "--symbol", "Holder<bool>"]);
    await t.cli("query --method symbolSearch --symbol Holder");
});
