import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query project-files");
    await t.cli("query project-files --filter source");
    await t.cli("query file-deps main.cpp --direction includes");
    await t.cli("query file-deps a.h --direction includers");
    await t.cli("query impact-analysis a.h");
    await t.cli("query counterparts a.h");
});
