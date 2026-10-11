import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(
    "includes resolve beside the link",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        // The database names the link; its include must resolve beside the
        // link, where the build sees it, not beside the file it points to.
        t.disk.symlink("src/main.cpp", "real/main.cpp");
        t.disk.database({ "src/main.cpp": [] });
        const s = await t.start();
        expect(await s.errors("src/main.cpp")).toEqual([]);
    },
);
