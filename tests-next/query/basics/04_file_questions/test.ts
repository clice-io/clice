import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query --method projectFiles");
    await t.cli("query --method projectFiles --filter source");
    await t.cli("query --method fileDeps --path main.cpp --direction includes");
    await t.cli("query --method fileDeps --path a.h --direction includers");
    await t.cli("query --method impactAnalysis --path a.h");
    await t.cli("query --method counterparts --path a.h");
});
