import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

async function builds(s: Serve): Promise<{ compile: number | undefined; pch: number | undefined }> {
    await s.sync();
    const counts = (await s.counts()).files["main.cpp"];
    return { compile: counts?.compile, pch: counts?.pch };
}

test("reopen reuses the pch", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.close("main.cpp");
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual({ compile: 2, pch: 1 });
});

test("edited while closed", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.close("main.cpp");
    t.disk.edit("main.cpp", { replace: "return v.a;", with: "return v.b;" });
    await t.show("reopened", s.errors("main.cpp"));
});

test("header changed while closed", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.close("main.cpp");
    t.disk.write("header.h", "#pragma once\n\nstruct V2 {\n    int b;\n};\n");
    t.disk.edit("main.cpp", { replace: "V1 v;\n    return v.a;", with: "V2 v;\n    return v.b;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual({ compile: 2, pch: 2 });
});

test("closed file answers nothing", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.close("main.cpp");
    const reply = await s.hover(t.at("main.cpp", "|main()")).then(
        () => "answered",
        (error: unknown) => (error instanceof Error ? error.message : String(error)),
    );
    await t.show("hover", reply);
});
