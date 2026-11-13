import { test } from "@clice/tools/testing";

// Questions clice cannot answer as asked: each says what is wrong and
// exits 1, or 2 for arguments the question does not take.
test({ errors: ["No index cache at"] }, async (t) => {
    await t.cli("query symbol-search add");
    // An argument the question does not take is reported before the index
    // is opened.
    await t.cli("query call-graph add --direction caller");
    await t.cli("index");
    for (const question of [
        "call-graph add --direction sideways",
        "project-files --filter sources",
        "type-hierarchy add --direction up",
        "definition nope",
        "document-symbols gone.cpp",
        "file-deps gone.cpp",
        "definition main.cpp:0",
        "definition gone.cpp:2",
        "definition add main.cpp",
        "symbol-search add main.cpp",
        "definition",
        "bogus",
        "defintion add",
    ]) {
        await t.cli(`query ${question}`);
    }
    // A place is held to the kinds asked for; a kind must be one clice knows.
    await t.cli([
        "query",
        "definition",
        `${t.place(t.at("main.cpp", "int |compute"))} kind:variable`,
    ]);
    await t.cli(["query", "symbol-search", "add kind:Fnction"]);
});
