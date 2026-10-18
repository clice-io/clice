import { test } from "@clice/tools/testing";

// The importer instantiates the module's template with types of its own,
// internal and function-local: the module's unit writes the template's
// entries, the importer what the instantiations read there.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references main.cpp:5:12 --include-declaration");
    await t.cli("query references main.cpp:15:16 --include-declaration");
});
