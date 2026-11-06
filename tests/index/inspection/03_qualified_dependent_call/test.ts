import { test } from "@clice/tools/testing";

// util.h calls ns::erase_if on a dependent container. It brings in the vec
// overload itself, through algo.h; b.cpp brings in the list one before it,
// which util.h includes only after the call. util.h means the same in both
// units: one variant, which names the overload it sees on its own.
test(async (t) => {
    await t.cli("index -j 1");
    await t.cli("index file util.h");
});
