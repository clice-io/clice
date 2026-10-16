import { test } from "@clice/tools/testing";

test(
    "batch indexing finds a linked build tree",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("project/sub/build", "outside/build");
        await t.cli("index --workspace project --jobs 2");
    },
);
