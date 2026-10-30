import { test } from "@clice/tools/testing";

// ws/link.cpp links to a file outside the workspace: named, the run fails;
// left to the build's own files, it is left out. Nothing is rewritten.
test(
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("ws/link.cpp", "outside/main.cpp");
        await t.cli("format ws/link.cpp --workspace ws");
        await t.cli("format --workspace ws");
    },
);
