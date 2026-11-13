import { test } from "@clice/tools/testing";

// The importer instantiates the module's template with types of its own,
// internal and function-local: the module's unit writes the template's
// entries, the importer what the instantiations read there. It also reads
// the member of a class a module function declares and returns.
test(async (t) => {
    await t.cli("index");
    await t.cli(
        `query references ${t.place(t.at("main.cpp", "double |radius;"))} --include-declaration`,
    );
    await t.cli(
        `query references ${t.place(t.at("main.cpp", "double |radius = 0"))} --include-declaration`,
    );
    await t.cli(`query definition ${t.place(t.at("main.cpp", "make().|radius"))}`);
    await t.cli(
        `query references ${t.place(t.at("main.cpp", "make().|radius"))} --include-declaration`,
    );
});
