import { test } from "@clice/tools/testing";

// A module that fails to build: closing it and a file importing it clears
// what each showed.
test(async (t) => {
    const s = await t.start();
    s.open("deps.cppm");
    s.open("main.cpp");
    await t.show("opened", {
        "deps.cppm": s.pushes("deps.cppm"),
        "main.cpp": s.pushes("main.cpp"),
    });
    s.close("deps.cppm");
    s.close("main.cpp");
    await t.show("closed", {
        "deps.cppm": s.pushes("deps.cppm"),
        "main.cpp": s.pushes("main.cpp"),
    });
});
