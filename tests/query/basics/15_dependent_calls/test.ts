import { test } from "@clice/tools/testing";

// process and the generic lambda are instantiated once each, with A and
// with B; an open main.cpp resolves the dependent calls through those
// instantiations (navigation/references/05_dependent_calls).
test(async (t) => {
    await t.cli("index");
    await t.cli("query references A::foo");
    await t.cli("query references B::bar");
});
