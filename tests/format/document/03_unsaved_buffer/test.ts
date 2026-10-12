import { test } from "@clice/tools/testing";

// The editor formats the buffer as the user typed it, not the file on disk.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    s.edit("main.cpp", {
        replace: "int add(int a, int b) { return a + b; }",
        with: "int   add(int a,int b){return a+b;}",
    });
    await t.show("edited", s.formatting("main.cpp"));
});
