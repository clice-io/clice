import { test } from "@clice/tools/testing";

// main.cpp looks answer.h up as embedded data before it includes it: the
// parse then holds the header as its bytes, byte order mark and all, while
// the disk's record of a file hashes its text. The disk holds those bytes
// all the same: the unit is indexed once, not over and over.
test(async (t) => {
    await t.cli("index");
});
