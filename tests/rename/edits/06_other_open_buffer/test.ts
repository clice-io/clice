import { test } from "@clice/tools/testing";

// Another open file's unsaved call is renamed in its buffer too. The
// files open once indexed: a header whose includers are all open before
// it is indexed stays unindexed (rename/edits/05).
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("main.cpp");
    s.open("other.cpp");
    s.edit("other.cpp", {
        after: "return compute(3); }\n",
        insert: "int later() { return compute(9); }\n",
    });
    await s.diagnostics("other.cpp");
    await t.show("renamed", s.rename(t.at("main.cpp", "int |compute(int x)"), "evaluate"), {
        parts: ["versions"],
    });
});
