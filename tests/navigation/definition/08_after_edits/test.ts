import { test } from "@clice/tools/testing";

// Navigation follows the buffer as it is edited: a function moved down by
// lines typed above it, and one just added, are found where they now are.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.sync();
    await t.show("before", s.definition(t.at("main.cpp", "return |area(")));
    s.edit("main.cpp", {
        before: "int area(",
        insert: "int volume(int side) {\n    return side * side * side;\n}\n\n",
    });
    s.edit("main.cpp", { replace: "return area(1, 2);", with: "return area(1, 2) + volume(3);" });
    await t.show("moved", s.definition(t.at("main.cpp", "return |area(1")));
    await t.show("added", s.definition(t.at("main.cpp", "+ |volume(")));
});
