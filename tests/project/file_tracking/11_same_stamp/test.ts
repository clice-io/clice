import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("same stamp cdb rewrite applied", async (t) => {
    // A stamp the filesystem cannot vouch for: a time not safely in the
    // past, so an unchanged stat proves nothing about the bytes.
    t.disk.database();
    const stamp = new Date(Date.now() + 3_600_000);
    t.disk.touch("compile_commands.json", stamp);
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("old", s.errors("main.cpp"));
    expect(await s.poll("cdb", { force: false })).toBe(0);

    // In place, the same length, the same time: only the content tells.
    const text = t.disk.read("compile_commands.json");
    t.disk.write("compile_commands.json", text.replace("-DOLD", "-DNEW"));
    t.disk.touch("compile_commands.json", stamp);
    expect(t.disk.read("compile_commands.json").length).toBe(text.length);
    expect(t.disk.mtime("compile_commands.json").getTime()).toBe(stamp.getTime());

    // The new content settles like any rewrite: seen on two polls.
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
    expect(await s.errors("main.cpp")).toEqual([]);
});
