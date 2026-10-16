import { test } from "@clice/tools/testing";

// util/strops.h serves core alone: moved there, util has one header less
// and the move is no longer proposed. Moves given together are evaluated
// together.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules");
    await t.cli("analyze modules --move util/strops.h=core");
    await t.cli("analyze modules --move util/strops.h=core --move util/clock.h=meta");
});
