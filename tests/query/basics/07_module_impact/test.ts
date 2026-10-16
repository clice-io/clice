import { test } from "@clice/tools/testing";

// What a change to a file affects, through includes and through a module
// that includes it: use.cpp imports shapes, whose interface includes
// base.h.
test(async (t) => {
    await t.cli("index");
    await t.cli("query impact-analysis base.h");
    await t.cli("query impact-analysis shapes.cppm");
    await t.cli("query file-deps base.h --direction includers --depth 0");
    await t.cli("query file-deps main.cpp --direction includes --depth 0");
});
