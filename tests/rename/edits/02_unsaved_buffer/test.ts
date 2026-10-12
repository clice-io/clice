import { test } from "@clice/tools/testing";

// The rename reads the open buffer, not the disk: a call typed but not
// saved is renamed, and the buffer's edits carry its version; the files
// not open are edited on disk.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    s.edit("main.cpp", { before: "int main()", insert: "int more() { return compute(5); }\n" });
    await s.diagnostics("main.cpp");
    await t.show("renamed", s.rename(t.at("main.cpp", "int |compute(int x)"), "evaluate"), {
        parts: ["versions"],
    });
});
