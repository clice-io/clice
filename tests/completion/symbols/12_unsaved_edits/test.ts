import { test } from "@clice/tools/testing";

// Completion reads the file's own buffer as typed: a local just declared
// completes before anything is saved. Another open file's buffer reaches it
// once saved, as every compile of a file sees its headers as on disk.
test("unsaved buffers", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.sync();
    s.edit("main.cpp", {
        replace: "return w.width;",
        with: "int fresh_total = w.width;\n    return fre;",
    });
    await t.show("local", s.completion(t.at("main.cpp", "return fre|;")));
    s.open("widget.h");
    await s.sync();
    s.edit("widget.h", { after: "int width;\n", insert: "    int height;\n" });
    s.edit("main.cpp", { replace: "return fre;", with: "return w.;" });
    await t.show("header unsaved", s.completion(t.at("main.cpp", "return w.|;")));
    s.save("widget.h");
    await t.show("header saved", s.completion(t.at("main.cpp", "return w.|;")));
});
