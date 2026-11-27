import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The lookup of inc/gen/../h.h fails, as the compiler's does while inc/gen is
// missing, and stays failed though inc/h.h exists: the PCH recording it is
// reused by every edit. (Windows folds `..` by text, and finds inc/h.h.)
test("include through a missing directory", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "return answer();", with: "return answer() + 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "return answer() + 1;", with: "return answer() + 2;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.pch).toBe(1);
});
