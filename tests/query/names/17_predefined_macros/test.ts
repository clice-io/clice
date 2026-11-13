import { test } from "@clice/tools/testing";

// FOO comes from the compile command (-DFOO=1) and __cplusplus from the
// compiler: no file defines them, and the answers name where each comes
// from.
test(async (t) => {
    await t.cli("index");
    await t.cli(`query references ${t.place(t.at("main.cpp", "= |FOO"))}`);
    await t.cli("query definition FOO");
    await t.cli("query symbol-search FOO");
    await t.cli("query definition __cplusplus");
    await t.cli(["query", "definition", `${t.place(t.at("main.cpp", "= |FOO"))} kind:function`]);
});
