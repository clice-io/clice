import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("broken module drops its pcm", async (t) => {
    const s = await t.start();
    expect(await s.errors("mid.cppm")).toEqual([]);
    // Leaf breaks while the server is down: its rebuild fails, and Mid
    // reports the import instead of compiling against the PCM of Leaf's
    // previous interface.
    await s.offline(() => {
        t.disk.edit("leaf.cppm", { replace: "return 1;", with: "return broken_in_leaf;" });
    });
    await t.show("mid.cppm", s.errors("mid.cppm"));
});
