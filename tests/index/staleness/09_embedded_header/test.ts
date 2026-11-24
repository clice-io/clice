import { test } from "@clice/tools/testing";

// main.cpp looks answer.h up as embedded data before it includes it: the
// parse then holds the header as its bytes, byte order mark and all, which
// the disk's hash of its text leaves out. Every run reads those bytes, so
// the unit is indexed again once, never over and over.
test(async (t) => {
    await t.cli("index");
});
