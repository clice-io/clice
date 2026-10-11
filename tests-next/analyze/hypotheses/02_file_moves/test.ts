import { test } from "@clice/tools/testing";

// util/strops.h serves core alone: moved there, util has one header less
// and the move is no longer proposed.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules");
    await t.cli("analyze modules --move util/strops.h=core");
});
