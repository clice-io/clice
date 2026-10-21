import { test } from "@clice/tools/testing";

// clang searches every -isystem directory before the ones the driver adds,
// and drops a directory named by both -I and -isystem from the -I list: the
// static_asserts hold and the links show the headers the compiler reads,
// while the scan resolves the same includes on its own.
test(async (t) => {
    const s = await t.start();
    await t.show("errors", s.errors("main.cpp"));
    await t.show("links", s.documentLinks("main.cpp"));
    await t.cli("scan --json");
});
