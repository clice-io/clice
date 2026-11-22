import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

const GENERATED = "#pragma once\ninline int generated() {\n    return 1;\n}\n";

async function pchBuilds(s: Serve, file: string): Promise<number | undefined> {
    await s.sync();
    return (await s.counts()).files[file]?.pch;
}

// An error inside a header the preamble includes stands until the header
// changes, which the PCH's inputs see: a save builds nothing again.
test("error in a header", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("broken", s.errors("main.cpp"));
    s.edit("main.cpp", { replace: "other() + 0", with: "other() + 1" });
    s.save("main.cpp");
    await s.errors("main.cpp");
    expect(await pchBuilds(s, "main.cpp")).toBe(1);
});

// A framework directory records no place its lookup looked in: the header
// appearing there is found once the file is saved.
test("framework header appears", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("missing", s.errors("framework.cpp"));
    t.disk.write("frameworks/Gen.framework/Headers/gen.h", GENERATED);
    s.save("framework.cpp");
    expect(await s.errors("framework.cpp")).toEqual([]);
    expect(await pchBuilds(s, "framework.cpp")).toBe(2);
});

// The build running when the file is saved looked before the header
// appeared: the save overtakes it, and the build after it finds the header.
test("save during a build", async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await s.errors("framework.cpp");
    const build = await s.park("pch", "framework.cpp", { at: "reply" });
    t.disk.edit("other.h", { replace: "return 1;", with: "return 2;" });
    const rebuilt = s.errors("framework.cpp");
    await build.reached();
    t.disk.write("frameworks/Gen.framework/Headers/gen.h", GENERATED);
    s.save("framework.cpp");
    await build.release();
    void rebuilt.catch(() => undefined);
    expect(await s.errors("framework.cpp")).toEqual([]);
});
