import { test } from "@clice/tools/testing";

// Questions clice cannot answer as asked: each says what is wrong and
// exits 1.
test(async (t) => {
    await t.cli("query --method symbolSearch --symbol add");
    await t.cli("index");
    for (const question of [
        "callGraph --symbol add --direction sideways",
        "definition --symbol nope",
        "documentSymbols --path gone.cpp",
        "fileDeps --path gone.cpp",
        "definition --symbol main.cpp:0",
        "definition --symbol gone.cpp:2",
        "definition --symbol add --path main.cpp",
        "symbolSearch --symbol add --path main.cpp",
        "definition",
        "bogus",
    ]) {
        await t.cli(`query --method ${question}`);
    }
    // A place is held to the kinds asked for; a kind must be one clice knows.
    await t.cli(["query", "--method", "definition", "--symbol", "main.cpp:4:5 kind:variable"]);
    await t.cli(["query", "--method", "symbolSearch", "--symbol", "add kind:Fnction"]);
});
