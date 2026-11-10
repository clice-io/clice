import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(
    "parent segment past a symlinked directory",
    {
        except: {
            win32: "Windows takes `..` off before it follows the link, and so does its compiler: the include is not found there",
        },
    },
    async (t) => {
        t.disk.symlink("inc", "vendor/real");
        const s = await t.start();
        // `..` climbs from the link's target, as clang does.
        expect(await s.errors("main.cpp")).toEqual([]);
        await t.show("hosts", s.contexts("vendor/common.h"));
    },
);
