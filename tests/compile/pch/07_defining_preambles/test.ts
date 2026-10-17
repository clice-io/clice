import { test } from "@clice/tools/testing";

// a.cpp and b.cpp open with the same preamble, which defines a macro.
test(async (t) => {
    const s = await t.start();
    await s.errors("a.cpp");
    await s.errors("b.cpp");
    await s.indexed();
    await t.show("definition", s.definition(t.at("b.cpp", "x() + |LIMIT")));
    await t.show("hover", s.hover(t.at("b.cpp", "#define |LIMIT")));
    await t.show("references", s.references(t.at("b.cpp", "#define |LIMIT")));
});
