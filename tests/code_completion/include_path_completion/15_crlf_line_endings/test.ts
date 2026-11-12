import { test } from "@clice/tools/testing";

// Saved with Windows line endings by the test: a header name on a line after
// another completes as it does between newlines, its carriage return no part
// of the name or the text after the cursor.
test(async (t) => {
    t.disk.write("main.cpp", t.disk.read("main.cpp").replaceAll("\n", "\r\n"));
    const s = await t.start();
    s.open("main.cpp");
    await t.show("angled", s.completion(t.at("main.cpp", "#include <snap|>")), {
        parts: ["insert", "edit"],
    });
    await t.show("open", s.completion(t.at("main.cpp", '#include "snap|')), {
        parts: ["insert", "edit"],
    });
});
