import { test } from "@clice/tools/testing";

test({ errors: ["--runs prints times, --json the graph"] }, async (t) => {
    await t.cli("scan --json --runs 2");
    await t.cli("scan --runs 0");
});
