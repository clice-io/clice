import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

// main.cpp's preamble PCH is built over the target's. The chain follows the
// target's PCH when it is built again, fails to build or is gone.

const WRITER = "build/CMakeFiles/app.dir/cmake_pch.hxx.cxx";
const PCH = ".clice/cache/*/pch/*.pch";
const config = { project: { enable_indexing: false } };

async function builds(s: Serve): Promise<{ base: number; top: number }> {
    await s.sync();
    const { files } = await s.counts();
    return { base: files[WRITER]?.pch ?? 0, top: files["main.cpp"]?.pch ?? 0 };
}

/// The target's PCH on disk: the older of the two.
function base(t: TestContext): string {
    const pchs = t.disk.list(PCH);
    expect(pchs).toHaveLength(2);
    return [...pchs].sort((a, b) => t.disk.mtime(a).getTime() - t.disk.mtime(b).getTime())[0] ?? "";
}

test("a header of the base changes", async (t) => {
    const s = await t.start({ config });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("include/base.h", { replace: "area(int side)", with: "area(int side, int scale)" });
    await t.show("edited", s.errors("main.cpp"));
    expect(await builds(s)).toEqual({ base: 2, top: 2 });
    t.disk.edit("include/base.h", { replace: "area(int side, int scale)", with: "area(int side)" });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual({ base: 3, top: 3 });
});

test("two edits in a row", async (t) => {
    const s = await t.start({ config });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("include/base.h", { replace: "area(int side)", with: "area(int side, int scale)" });
    t.disk.edit("include/base.h", {
        replace: "area(int side, int scale)",
        with: "area(long side)",
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("definition", s.definition(t.marker("use")));
});

test("the base fails to build", async (t) => {
    const s = await t.start({ config });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("include/base.h", { replace: "side * side", with: "side * missing" });
    await t.show("broken", s.diagnostics("main.cpp"));
    // Built without the base, over the header's text.
    expect(await builds(s)).toEqual({ base: 2, top: 2 });
    t.disk.edit("include/base.h", { replace: "side * missing", with: "side * side" });
    expect(await s.errors("main.cpp")).toEqual([]);
    // The header reads as the first build had it: both PCHs of then serve.
    expect(await builds(s)).toEqual({ base: 2, top: 2 });
});

test("the base is gone while the server runs", async (t) => {
    const s = await t.start({ config });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.rm(base(t));
    s.edit("main.cpp", { replace: "twice_area(2)", with: "twice_area(5)" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("definition", s.definition(t.at("main.cpp", "|area(3)")));
});

test("the base is gone over a restart", async (t) => {
    const s = await t.start({ config });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.offline(() => {
        t.disk.rm(base(t));
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual({ base: 1, top: 1 });
});
