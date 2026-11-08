import { test } from "@clice/tools/testing";

// util.h calls lib::erase_if without including what declares it: the
// command includes prelude.h ahead of every file. util.h compiled alone
// with that command sees the overload, and keeps the candidate reference and
// the call edge.
test(async (t) => {
    await t.cli("index -j 1");
    await t.cli("index file util.h");
});
