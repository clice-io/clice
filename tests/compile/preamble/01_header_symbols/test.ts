import { answers, test } from "@clice/tools/testing";

const SERVER = { server: "navigation answers from the server's index and preambles" };

answers("include", async (a) => {
    await a.show("include", a.hover(a.marker("include")));
    // Link ranges are half-open: just past the closing quote is no link.
    await a.show("past", a.hover(a.marker("past")));
    await a.show("get", a.hover(a.marker("get")));
});

// Background indexing is off: only what the precompiled preamble recorded
// knows the header and the macro. The preamble skips the header's function
// bodies: the call in bar() waits for the index.
answers("unindexed", { paths: SERVER }, async (a) => {
    await a.show("call", {
        definition: a.definition(a.marker("call")),
        references: a.references(a.marker("call")),
    });
    await a.show("macro", a.definition(a.marker("macro")));
});

// The header's entries are in the index, from other.cpp, and in what main.cpp
// precompiled: each shows once.
answers("indexed", { indexing: true, paths: SERVER }, async (a) => {
    await a.show("call", a.references(a.marker("call")));
});

test("header edit refreshes", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const call = t.at("main.cpp", "|foo();");
    await s.errors("main.cpp");
    await t.show("before", s.definition(call));
    // The include line is unchanged, so is the PCH's key: the header's
    // change alone must refresh what the preamble serves. foo moves a line
    // down; an answer from before would point at the new first line.
    t.disk.edit("foo.h", { replace: "inline void foo", with: "// moved\ninline void foo" });
    await s.errors("main.cpp");
    await t.show("after", s.definition(call));
});
