import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

// main.cpp includes pre.h first, which is precompiled with base.h, and
// late.h after its code. Every change below happens on disk behind the
// server's back: no buffer, no save unless the test says so.

async function builds(
    s: Serve,
    file: string,
): Promise<{ compile: number | undefined; pch: number | undefined }> {
    await s.sync();
    const counts = (await s.counts()).files[file];
    return { compile: counts?.compile, pch: counts?.pch };
}

test("precompiled header edit", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    const original = t.disk.read("pre.h");
    t.disk.edit("pre.h", { replace: "int pre()", with: "int pre_renamed()" });
    await t.show("renamed", s.errors("main.cpp"));
    expect(await builds(s, "main.cpp")).toEqual({ compile: 2, pch: 2 });
    // Removed and written anew, as a tool replacing the file does.
    t.disk.rm("pre.h");
    t.disk.write("pre.h", original);
    expect(await s.errors("main.cpp")).toEqual([]);
});

test("transitive header edit", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("base.h", { replace: "return 1;", with: "return;" });
    await t.show("broken", s.errors("main.cpp"));
});

test("body header edit", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("late.h", { replace: "int late()", with: "int late_renamed()" });
    await t.show("renamed", s.errors("main.cpp"));
    // late.h is no part of the PCH.
    expect(await builds(s, "main.cpp")).toEqual({ compile: 2, pch: 1 });
});

test("both edits compile once", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("pre.h", { replace: "int pre()", with: "int pre_renamed()" });
    t.disk.edit("late.h", { replace: "int late()", with: "int late_renamed()" });
    // The request's own check finds both changes before the compile it
    // starts, not one in the middle of it.
    await s.hover(t.at("main.cpp", "|main()"));
    expect(await builds(s, "main.cpp")).toEqual({ compile: 2, pch: 2 });
    await t.show("renamed", s.errors("main.cpp"));
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.publish).toBe(2);
});

test("shared header edit", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("other.cpp")).toEqual([]);
    t.disk.edit("pre.h", { replace: "int pre()", with: "int pre_renamed()" });
    await t.show("renamed", { main: s.errors("main.cpp"), other: s.errors("other.cpp") });
});

test("same bytes are no change", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const call = t.at("main.cpp", "return |pre();");
    await s.hover(call);
    expect(await builds(s, "main.cpp")).toEqual({ compile: 1, pch: 1 });
    // A checkout there and back moves only the times.
    for (const file of ["pre.h", "late.h"]) {
        t.disk.write(file, t.disk.read(file));
        t.disk.touch(file, new Date(Date.now() + 5_000));
    }
    await s.hover(call);
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s, "main.cpp")).toEqual({ compile: 1, pch: 1 });
    // The PCH keeps serving what it precompiled.
    s.edit("main.cpp", { replace: "return pre();", with: "Widget w;\n    w.\n    return pre();" });
    await t.show("member", s.completion(t.at("main.cpp", "w.|")));
    expect(await builds(s, "main.cpp")).toEqual({ compile: 1, pch: 1 });
});

test("backdated edit", async (t) => {
    // rsync -t and git-restore-mtime give new bytes an older time.
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    const before = t.disk.mtime("pre.h");
    t.disk.edit("pre.h", { replace: "int pre()", with: "int pre_renamed()" });
    t.disk.touch("pre.h", new Date(before.getTime() - 100_000));
    await t.show("renamed", s.errors("main.cpp"));
});

test("saved at once", async (t) => {
    // Saved in the second the compile read it: no time tells them apart.
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.edit("pre.h", { replace: "int pre()", with: "int pre_renamed()" });
    s.save("pre.h");
    await t.show("renamed", s.errors("main.cpp"));
});
