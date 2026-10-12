import { test } from "@clice/tools/testing";

// Two headers declare widen and none defines it. The index is built twice,
// one worker taking the units in the database's order, first a.cpp then
// b.cpp, then the other way round: which file provides widen should not
// depend on that order.
test(async (t) => {
    t.disk.database({ "app/a.cpp": [], "util/b.cpp": [] });
    await t.cli("index --workers 1");
    await t.cli("analyze modules --view file --file util/b.cpp");
    t.disk.rm(".clice");
    t.disk.database({ "util/b.cpp": [], "app/a.cpp": [] });
    await t.cli("index --workers 1");
    await t.cli("analyze modules --view file --file util/b.cpp");
});
