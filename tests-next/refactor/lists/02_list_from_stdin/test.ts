import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol compute --to evaluate");
    await t.cli("index --workers 2");
    await t.cli("refactor rename --spec - --dry-run", { input: "  evaluate   =>   compute  \n" });
});
