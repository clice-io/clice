import { test } from "@clice/tools/testing";

test("the module reports its errors", async (t) => {
    const s = await t.start();
    await t.show("bad.cppm", s.errors("bad.cppm"));
});

test("the importer reports the import", async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.errors("main.cpp"));
    // The importer still follows its own edits.
    s.edit("main.cpp", { replace: "return bad();", with: "return bad() + 1;" });
    await t.show("after an edit", s.errors("main.cpp"));
});

test("the importer still completes", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    s.edit("main.cpp", { replace: "return bad();", with: "int local = 1;\n    return loc;" });
    await t.show("completion", s.completion(t.at("main.cpp", "return loc|;")));
});
