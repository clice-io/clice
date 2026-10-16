import { test } from "@clice/tools/testing";

// Questions clice cannot answer as asked: each says what is wrong and
// exits 1, or 2 for arguments the question does not take.
test(async (t) => {
    await t.cli("query symbol-search add");
    await t.cli("index");
    for (const question of [
        "call-graph add --direction sideways",
        "definition nope",
        "document-symbols gone.cpp",
        "file-deps gone.cpp",
        "definition main.cpp:0",
        "definition gone.cpp:2",
        "definition add main.cpp",
        "symbol-search add main.cpp",
        "definition",
        "bogus",
    ]) {
        await t.cli(`query ${question}`);
    }
    // A place is held to the kinds asked for; a kind must be one clice knows.
    await t.cli(["query", "definition", "main.cpp:4:5 kind:variable"]);
    await t.cli(["query", "symbol-search", "add kind:Fnction"]);
});
