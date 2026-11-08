import { test } from "@clice/tools/testing";

// util.h imports lib and calls lib::erase_if on a dependent container;
// wrap.h does the same through config.h, which imports lib for it; relay.h
// imports facade, which re-exports api. Each header sees the overload on
// its own and keeps the candidate reference and the call edge.
test(async (t) => {
    await t.cli("index -j 1");
    await t.cli("index file util.h");
    await t.cli("index file wrap.h");
    await t.cli("index file relay.h");
});
