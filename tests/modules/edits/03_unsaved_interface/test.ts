import { test } from "@clice/tools/testing";

// The interface gains a function in its editor buffer, unsaved; the
// importer, open beside it, calls the function.
test("unsaved", async (t) => {
    const s = await t.start();
    s.open("math.cppm");
    s.open("main.cpp");
    s.edit("math.cppm", {
        after: "}\n",
        insert: "\nexport int twice(int a) {\n    return a * 2;\n}\n",
    });
    s.edit("main.cpp", { replace: "return add(1, 2);", with: "return add(1, 2) + twice(3);" });
    await t.show("errors", s.errors("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "+ |twice(3)")));
});

test("saved", async (t) => {
    const s = await t.start();
    s.open("math.cppm");
    s.open("main.cpp");
    s.edit("math.cppm", {
        after: "}\n",
        insert: "\nexport int twice(int a) {\n    return a * 2;\n}\n",
    });
    s.save("math.cppm");
    s.edit("main.cpp", { replace: "return add(1, 2);", with: "return add(1, 2) + twice(3);" });
    await t.show("errors", s.errors("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "+ |twice(3)")));
});
