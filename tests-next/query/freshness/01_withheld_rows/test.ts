import { test } from "@clice/tools/testing";

// `clice query` reads the persisted index as it is: rows of a file the
// disk moved on from would point at text that moved, so they are withheld
// and the file is named in `stale`.
test(async (t) => {
    await t.cli("index");
    t.disk.edit("main.cpp", { before: "int main()", insert: "int extra() { return 7; }\n" });
    await t.cli("query --method definition --symbol compute");
    // A place in a file the index never held is unknown, the file named.
    t.disk.write("new.cpp", "int fresh() { return 1; }\n");
    await t.cli("query --method definition --symbol new.cpp:1:5");
    await t.cli("query --method documentSymbols --path main.cpp");
    await t.cli("query --method documentSymbols --path a.h");
    // The edited file's definitions no longer vouch for a pairing.
    await t.cli("query --method counterparts --path a.h");
});
