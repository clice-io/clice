import { test } from "@clice/tools/testing";

// A class template and its explicit specialization: the name alone names
// the template, the specialization is named with its arguments.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references Holder");
    await t.cli(["query", "references", "Holder<bool>"]);
    await t.cli("query symbol-search Holder");
});
