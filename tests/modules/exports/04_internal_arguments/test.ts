import { test } from "@clice/tools/testing";

// The importer instantiates the module's template with types of its own,
// internal and function-local: the module's unit writes the template's
// entries, the importer what the instantiations read there. It also reads
// the member of a class a module function declares and returns.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references main.cpp:5:12 --include-declaration");
    await t.cli("query references main.cpp:15:16 --include-declaration");
    await t.cli("query definition main.cpp:21:19");
    await t.cli("query references main.cpp:21:19 --include-declaration");
});
