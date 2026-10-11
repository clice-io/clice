import { test } from "@clice/tools/testing";

// A declaration added to an open header and used at once from a source
// including it: the source sees it only once the header is saved.
test(async (t) => {
    const s = await t.start();
    s.open("shapes.h");
    s.open("main.cpp");
    s.edit("shapes.h", {
        after: "int area(int width, int height);\n",
        insert: "int volume(int side);\n",
    });
    s.edit("main.cpp", { replace: "return area(1, 2);", with: "return area(1, 2) + volume(3);" });
    await t.show("errors", s.errors("main.cpp"));
    await t.show("added", s.definition(t.at("main.cpp", "+ |volume(")));
    s.save("shapes.h");
    await t.show("saved errors", s.errors("main.cpp"));
    await t.show("saved", s.definition(t.at("main.cpp", "+ |volume(")));
});
