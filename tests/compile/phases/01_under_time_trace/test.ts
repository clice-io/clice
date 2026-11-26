import { test } from "@clice/tools/testing";

// With CLICE_PERF=compile every compile runs under LLVM's time-trace
// profiler and logs where its time went inside clang: the document's, the
// index's, and the completion's, which compiles the whole buffer inside
// its own parse to learn what the call after the point instantiates the
// template with. Each answers as it does without the profiler, and so does
// one a fatal error ends inside an included file (deep.cpp).

test(async (t) => {
    const s = await t.start({ env: { CLICE_PERF: "compile" } });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("completion", s.completion(t.marker("member")));
    await s.indexed();
    await t.show("references", s.references(t.at("main.cpp", "int |held")));
    await t.show("cut short", s.diagnostics("deep.cpp"));
});
