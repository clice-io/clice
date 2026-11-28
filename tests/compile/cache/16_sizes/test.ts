import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Each size holds one PCH or PCM: the last built pushes out the one before.

test("a PCH past the size goes", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    expect(await s.errors("a.cpp")).toEqual([]);
    expect(await s.errors("b.cpp")).toEqual([]);
    expect(t.disk.list(".clice/precompiled/*/*/pch/*.pch")).toHaveLength(1);
    // a.cpp's PCH is built again, pushing out b.cpp's.
    s.edit("a.cpp", { replace: "return a();", with: "return a() + 1;" });
    expect(await s.errors("a.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(3);
    expect(t.disk.list(".clice/precompiled/*/*/pch/*.pch")).toHaveLength(1);
});

test("a PCM past the size goes", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    expect(await s.errors("x.cpp")).toEqual([]);
    expect(await s.errors("y.cpp")).toEqual([]);
    expect(t.disk.list(".clice/precompiled/*/*/pcm/*.pcm")).toHaveLength(1);
    s.edit("x.cpp", { replace: "return m1();", with: "return m1() + 1;" });
    expect(await s.errors("x.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["m1.cppm"]?.pcm).toBe(2);
    expect(t.disk.list(".clice/precompiled/*/*/pcm/*.pcm")).toHaveLength(1);
});
