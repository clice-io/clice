import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

/// The compiles and PCH builds of `file` once the server is idle.
async function builds(
    s: Serve,
    file: string,
): Promise<{ compile: number | undefined; pch: number | undefined }> {
    await s.sync();
    const counts = (await s.counts()).files[file];
    return { compile: counts?.compile, pch: counts?.pch };
}

test("body edits reuse the pch", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s, "main.cpp")).toEqual({ compile: 1, pch: 1 });
    await t.show("add", s.hover(t.marker("add")));

    s.edit("main.cpp", { replace: "return result;", with: "return result +;" });
    await t.show("body error", s.errors("main.cpp"));
    s.edit("main.cpp", { replace: "return result +;", with: "return result + 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    // A type of the precompiled header completes in the edited body.
    s.edit("main.cpp", { after: "return result + 1;\n}\n", insert: "Poi" });
    await t.show("completion", s.completion(t.at("main.cpp", "}\nPoi|")));
    expect(await builds(s, "main.cpp")).toEqual({ compile: 3, pch: 1 });
});

test("include edits rebuild the pch", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    const add = t.at("main.cpp", "int |add(int a");
    const before = s.hover(add);
    await t.show("add", before);
    s.edit(
        "main.cpp",
        { replace: '#include "common.h"\n', with: '#include "common.h"\n#include "extra.h"\n' },
        { replace: "return result;", with: "return result + extra();" },
    );
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s, "main.cpp")).toEqual({ compile: 2, pch: 2 });
    // The symbol moved a line down; its card reads the same.
    expect((await s.hover(add))?.text).toBe((await before)?.text);
    // Lines past the last include are no part of what is precompiled.
    for (const n of [1, 2, 3]) {
        s.edit("main.cpp", { after: '#include "extra.h"\n', insert: `// note ${String(n)}\n` });
        expect(await s.errors("main.cpp")).toEqual([]);
    }
    expect(await builds(s, "main.cpp")).toEqual({ compile: 5, pch: 2 });
});

test("no includes no pch", async (t) => {
    const s = await t.start();
    expect(await s.errors("no_includes.cpp")).toEqual([]);
    expect((await builds(s, "no_includes.cpp")).pch).toBe(0);
});
