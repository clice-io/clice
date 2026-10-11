import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("deleted source withdraws its rows", async (t) => {
    const s = await t.start();
    await s.indexed();
    const call = t.at("app/main.cpp", "|registry_count()");
    await t.show("before", s.definition(call));
    t.disk.rm("src/registry.cpp");
    await s.sync({ poll: true });
    await t.show("after", s.definition(call));
});

test("closed header edit reaches its includers", async (t) => {
    const s = await t.start();
    await s.indexed();
    t.disk.edit("include/registry.h", {
        replace: "int registry_count();",
        with: "int registry_size();",
    });
    await s.sync({ poll: true });
    await t.show("symbols", s.workspaceSymbols("registry_size"));
    expect(await s.errors("app/main.cpp")).not.toEqual([]);
});
