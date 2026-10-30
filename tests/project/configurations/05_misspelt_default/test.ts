import { test } from "@clice/tools/testing";

// A default_configuration naming no rule's tag is an error on clice.toml,
// and the project serves nothing until it is fixed: the undeclared name in
// main.cpp goes unreported.
test("a misspelt default is an error", { errors: ["Invalid clice\\.toml .*relase"] }, async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("menu", s.configurations());
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
