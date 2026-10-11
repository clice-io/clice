import { test } from "@clice/tools/testing";

// A crash building a preamble is shared: a document with the same preamble
// learns it without a crash of its own, and the fix, saved, brings the file
// back.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

test(async (t) => {
    const s = await t.start({
        config: QUIET,
        crash: { request: "buildPch", file: "poison.cpp", pragma: true },
    });
    const add = (file: string) => t.at(file, "int |add");
    await t.show("healthy", s.diagnostics("healthy.cpp"));
    s.open("poison.cpp");
    await t.show("hover", s.hover(add("poison.cpp")));
    await t.show("crashes", await s.crashes());
    s.open("twin.cpp");
    await t.show("twin hover", s.hover(add("twin.cpp")));
    await t.show("twin pushes", s.pushes("twin.cpp"));
    await t.show("crashes after the twin", await s.crashes());
    await t.show("healthy hover", s.hover(add("healthy.cpp")));
    s.edit("poison.cpp", { remove: "#pragma clang __debug crash\n" });
    s.save("poison.cpp");
    await t.show("hover after the fix", s.hover(add("poison.cpp")));
    await t.show("pushes", s.pushes("poison.cpp"));
});
