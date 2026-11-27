import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The lookup through `gen/..` fails while gen is missing, as the compiler's
// does, and stays failed: the PCH recording it is reused by every edit.
test("search directory through a missing one", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "return answer();", with: "return answer() + 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "return answer() + 1;", with: "return answer() + 2;" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["main.cpp"]?.pch).toBe(1);
});
