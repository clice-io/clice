import { test } from "@clice/tools/testing";

// Another open file's unsaved call is renamed in its buffer too.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    s.open("other.cpp");
    await s.indexed();
    s.edit("other.cpp", {
        after: "return compute(3); }\n",
        insert: "int later() { return compute(9); }\n",
    });
    await s.diagnostics("other.cpp");
    await t.show("renamed", s.rename(t.at("main.cpp", "int |compute(int x)"), "evaluate"), {
        parts: ["versions"],
    });
});
