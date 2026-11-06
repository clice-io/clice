import { test } from "@clice/tools/testing";

// Read-only at first, the file is compiled from its first completion on,
// without an edit: its background run then brings its unused includes.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    s.open("main.cpp", { pull: false });
    await s.indexed();
    await s.completion(t.at("main.cpp", "return us|ed()"));
    await s.indexed();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
});
