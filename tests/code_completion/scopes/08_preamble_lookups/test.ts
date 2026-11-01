import { answers, test, type AnswersContext } from "@clice/tools/testing";

// The names of lib.h come to the server's compile from the preamble's
// index, which loads just them where the parse stands at the point: through
// using-declarations and using-directives, a namespace's own names, an inline
// namespace's, an alias's and a macro's, in a function body, right inside a
// namespace and after a requirement's braces; of internal linkage too. An
// arrow after a call accesses a member.
async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers("default", items);
answers("configured", { config: { code_completion: { all_scopes: false } } }, items);

// Before the document's first compile the completion builds the PCH, whose
// index names the preamble's.
test("before the first compile", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    s.open("main.cpp", { pull: false });
    await t.show("names", s.completion(t.at("main.cpp", "return lib::helper|")), {
        parts: ["insert"],
    });
});

// An include added since the held compile: the completion's own PCH, and its
// index, hold its names.
test("an include added before the compile", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("main.cpp");
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.edit("main.cpp", { after: '#include "lib.h"\n', insert: '#include "extra.h"\n' });
    await t.show("names", s.completion(t.at("main.cpp", "return lib::helper|")), {
        parts: ["insert"],
    });
    await park.release();
});

// A namespace's using-declaration may bring in any name: past the names the
// index offers, its list is incomplete, unlike one bringing in none.
test("a namespace bringing in names", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("main.cpp");
    await t.show("brings in", s.completion(t.at("main.cpp", "return app::|helper")), {
        parts: ["insert"],
    });
    await t.show("brings in none", s.completion(t.at("main.cpp", "return lib::|helper")), {
        parts: ["insert"],
    });
});

// A header edited since the document's compile: the completion's PCH, built
// anew, holds names the index of the one before does not.
test("a header edited before the compile", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("main.cpp");
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("lib.h");
    s.edit("lib.h", { after: "int helper_value(int x);\n", insert: "int helper_added();\n" });
    s.save("lib.h");
    await t.show("names", s.completion(t.at("main.cpp", "return lib::helper|")), {
        parts: ["insert"],
    });
    await park.release();
});
