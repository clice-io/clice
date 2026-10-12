import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

function database(t: TestContext, feature: boolean): void {
    t.disk.database({
        "zsrc/lib.cpp": feature ? ["-DFEATURE", "-Iinclude"] : ["-Iinclude"],
        "include/near.cpp": [],
        "tools/tool.c": ["-std=c17", "-DTOOL"],
    });
}

test("nearby unit lends its command", async (t) => {
    const s = await t.start();
    // The sibling unit's -DFEATURE applies, and a working borrow is no news.
    t.disk.write("zsrc/new.cpp", gated("FEATURE"));
    expect(await s.diagnostics("zsrc/new.cpp")).toEqual([]);
    // The header sits under lib.cpp's -Iinclude: that unit lends, not the
    // nearer include/near.cpp.
    t.disk.write("include/api/extra.h", `#pragma once\n${gated("FEATURE")}`);
    expect(await s.errors("include/api/extra.h")).toEqual([]);
    // C files borrow from C units only.
    t.disk.write("zsrc/plain.c", gated("TOOL"));
    expect(await s.errors("zsrc/plain.c")).toEqual([]);
    t.disk.write("elsewhere/lone.c", gated("FEATURE"));
    await t.show("lone.c", s.errors("elsewhere/lone.c"));

    // The lender's command changes: the borrower follows.
    database(t, false);
    expect(await s.poll("cdb")).toBe(1);
    await t.show("lender changed", s.errors("zsrc/new.cpp"));
    database(t, true);
    expect(await s.poll("cdb")).toBe(1);
    expect(await s.errors("zsrc/new.cpp")).toEqual([]);
});

test("inspect borrows the same way", async (t) => {
    t.disk.write("zsrc/new.cpp", gated("FEATURE"));
    await t.cli("inspect hover zsrc/new.cpp");
});
