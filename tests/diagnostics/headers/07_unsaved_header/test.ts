import { test } from "@clice/tools/testing";

// The header's unsaved buffer declares the function main.cpp calls.
test(async (t) => {
    const s = await t.start();
    s.open("header.h");
    s.edit("header.h", {
        after: "inline int old_func() { return 1; }\n",
        insert: "inline int new_func() { return 2; }\n",
    });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
