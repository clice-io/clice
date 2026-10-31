import { test } from "@clice/tools/testing";

// A rename in the folder's project reads the unsaved buffer of a file the
// nested project serves: its edits are at the buffer's places and name the
// buffer's version.
test(async (t) => {
    const s = await t.start({ folders: ["repo"] });
    await s.indexed();
    s.open("repo/main.cpp");
    s.open("repo/sub/user.cpp");
    s.edit("repo/sub/user.cpp", {
        after: '#include "../api.h"\n',
        insert: "\nint later() { return compute(9); }\n",
    });
    await s.diagnostics("repo/sub/user.cpp");
    await t.show("renamed", s.rename(t.at("repo/main.cpp", "int |compute(int x)"), "evaluate"), {
        parts: ["versions"],
    });
});
