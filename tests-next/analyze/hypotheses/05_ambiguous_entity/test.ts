import { test } from "@clice/tools/testing";

// Traits alone names the template, an explicit specialization is named with
// its arguments; value, which the template and each specialization declare,
// names three declarations: a move cannot pick one.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules --move-entity Traits=core/traits2.h");
    await t.cli("analyze modules --move-entity value=core/traits2.h");
});
