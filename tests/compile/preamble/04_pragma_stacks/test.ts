import { test } from "@clice/tools/testing";

// Each file pushes a pragma state before its includes, which go into the
// precompiled preamble, and pops it in the body. Each compiles clean
// under clang.
test("pushed before the includes", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    for (const file of ["pack.cpp", "attribute.cpp", "visibility.cpp", "diagnostic.cpp"]) {
        await t.show(file, s.diagnostics(file));
    }
});
