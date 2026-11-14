import { test } from "@clice/tools/testing";

// A client without pull support: every compile of an open file publishes its
// diagnostics, and closing the file clears them. An edit alone compiles
// nothing; a save of text no compile has seen compiles it.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await t.show("opened", s.pushes("main.cpp"));
    s.edit("main.cpp", { replace: "int main() {", with: "int missing = 0; int main() {" });
    await t.show("edited", s.pushes("main.cpp"));
    s.save("main.cpp");
    await t.show("saved", s.pushes("main.cpp"));
    await s.hover(t.at("main.cpp", "int |main"));
    await t.show("asked", s.pushes("main.cpp"));
    s.close("main.cpp");
    await t.show("closed", s.pushes("main.cpp"));
});
