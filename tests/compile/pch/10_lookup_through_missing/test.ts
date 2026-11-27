import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The lookup of inc/gen/../h.h fails while inc/gen is missing, as the
// compiler's does, though inc/h.h exists: it stays failed, and the requests
// after the compile reuse it. (Windows folds `..` by text, and finds h.h.)
test("lookup through a missing directory", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    const compiled = (await s.counts()).files["main.cpp"]?.compile;
    await s.hover(t.at("main.cpp", "int |main"));
    await s.hover(t.at("main.cpp", "int |main"));
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.compile).toBe(compiled);
});
