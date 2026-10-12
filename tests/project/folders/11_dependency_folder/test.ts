import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("dependency folder borrows the application", async (t) => {
    // The header compiles in the context of the application including it.
    const s = await t.start({ folders: ["app", "dep"] });
    expect(await s.errors("dep/include/dep.h")).toEqual([]);
    await t.show("hosts", s.contexts("dep/include/dep.h"));
});
