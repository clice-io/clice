import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(
    "declared database appears",
    // Wrong: a database not built yet is expected, and load.cpp says so in
    // a warning; the loader adds this error line besides.
    { errors: ["Failed to read compilation database from .*/build/compile_commands.json"] },
    async (t) => {
        const s = await t.start();
        await t.show("before the build", s.diagnostics("main.cpp"));
        t.disk.database({ "main.cpp": ["-DFROM_BUILD"] }, "build");
        expect(await s.poll("cdb")).toBe(1);
        await t.show("after the build", s.diagnostics("main.cpp"));
    },
);
