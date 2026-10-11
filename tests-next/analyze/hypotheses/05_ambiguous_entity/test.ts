import { test } from "@clice/tools/testing";

// Traits names the template and each explicit specialization: a move
// cannot pick one.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --move-entity Traits=core/traits2.h");
});
