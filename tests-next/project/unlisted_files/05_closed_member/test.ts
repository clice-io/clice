import { test } from "@clice/tools/testing";

test("closed member stays indexed", async (t) => {
    const s = await t.start();
    s.open("src/a.cpp");
    t.disk.write("src/new.cpp", '#include "shared.h"\nint fresh_fn() { return shared_fn(); }\n');
    await s.diagnostics("src/new.cpp");
    s.save("src/new.cpp");
    s.close("src/new.cpp");
    await s.indexed();
    await t.show("indexed", {
        symbols: s.workspaceSymbols("fresh_fn"),
        references: s.references(t.at("src/a.cpp", "int |shared_fn")),
    });
});
