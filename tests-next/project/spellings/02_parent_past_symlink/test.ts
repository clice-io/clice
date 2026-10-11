import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(
    "parent segment past a symlinked directory",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("inc", "vendor/real");
        const s = await t.start();
        // `..` climbs from the link's target, as clang does.
        expect(await s.errors("main.cpp")).toEqual([]);
        await t.show("hosts", s.contexts("vendor/common.h"));
    },
);
