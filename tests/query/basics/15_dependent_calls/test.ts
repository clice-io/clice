import { test } from "@clice/tools/testing";

// process and the generic lambda are instantiated once each, with A and
// with B; the index answers these references through those instantiations,
// as an open main.cpp does (navigation/references/05_dependent_calls).
test(async (t) => {
    await t.cli("index");
    await t.cli("query references A::foo");
    await t.cli("query references B::bar");
});
