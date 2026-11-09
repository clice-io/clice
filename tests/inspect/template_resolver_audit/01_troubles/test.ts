import { test } from "@clice/tools/testing";

// Where the template resolver gives up or cannot decide, as
// `clice inspect template-resolver-audit` lists the places of a file: a
// lookup that reenters itself through a base, a member two bases declare,
// a partial choice an unknown value leaves open.
test(async (t) => {
    await t.cli(["inspect", "template-resolver-audit", "main.cpp"]);
});
