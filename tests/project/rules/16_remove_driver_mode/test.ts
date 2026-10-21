import { test } from "@clice/tools/testing";

// The rule takes cl mode out of the command: main.cpp compiles for the
// host, where _MSC_VER is not defined.
test(
    { except: { win32: "clang++ targets MSVC there, which defines _MSC_VER in either mode" } },
    async (t) => {
        const s = await t.start();
        await t.show("main.cpp", s.errors("main.cpp"));
    },
);
