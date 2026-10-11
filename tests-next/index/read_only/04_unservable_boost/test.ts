import { test } from "@clice/tools/testing";

// Files the index cannot serve fall back to compiling: orphan.cpp is on
// disk but in no database and included by nothing, so indexing refuses
// its guessed command; scratch.cpp is in the database but only an editor
// buffer holds it, and indexing reads the disk. The outline asked first
// waits for the failed attempt and is then answered by a compile.
test(async (t) => {
    t.disk.database({ "main.cpp": [], "scratch.cpp": [] });
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("orphan.cpp", { pull: false });
    await t.show("orphan", s.documentSymbols("orphan.cpp"));
    await t.show("orphan pushed", s.pushes("orphan.cpp"));
    s.open("scratch.cpp", { text: "int scratch() { return 2; }\n", pull: false });
    await t.show("scratch", s.documentSymbols("scratch.cpp"));
    await t.show("scratch pushed", s.pushes("scratch.cpp"));
});
