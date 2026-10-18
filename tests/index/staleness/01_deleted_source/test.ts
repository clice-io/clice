import { test } from "@clice/tools/testing";

test("deleted source withdraws its entries", async (t) => {
    const s = await t.start();
    await s.indexed();
    const call = t.at("app/main.cpp", "|registry_count()");
    await t.show("before", s.definition(call));
    t.disk.rm("src/registry.cpp");
    await s.sync({ poll: true });
    await t.show("after", {
        definition: s.definition(call),
        references: s.references(call),
        symbols: s.workspaceSymbols("registry_reset"),
    });
    // The persisted index drops the entries as well.
    await s.stop();
    await t.cli("query symbol-search registry_reset");
});

test("closed header edit reaches its includers", async (t) => {
    const s = await t.start();
    await s.indexed();
    t.disk.edit("include/registry.h", {
        replace: "int registry_count();",
        with: "int registry_size();",
    });
    await s.sync({ poll: true });
    await t.show("symbols", s.workspaceSymbols("registry_size"));
    await t.show("errors", s.errors("app/main.cpp"));
});
