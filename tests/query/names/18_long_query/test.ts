import { test } from "@clice/tools/testing";

// A query of many trigrams, decoded one posting list at a time, grows the
// cache that holds the earlier lists: the search still answers.
test(async (t) => {
    await t.cli("index");
    await t.cli("query symbol-search theQuickBrownFoxJumpsOverALazyDogWhileMyCatSleepsInWarmSun");
});
