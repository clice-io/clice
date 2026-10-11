import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("response file change reloads", async (t) => {
    // The rewrite below keeps the size: a time apart tells it from this one.
    t.disk.touch("flags.rsp", new Date("2020-01-01T00:00:00Z"));
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("other.cpp")).toEqual([]);

    t.disk.write("flags.rsp", "-DCHANGED\n");
    // The response file settles like the database.
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
    await t.show("main.cpp", s.errors("main.cpp"));
    // A unit without the response file is untouched.
    expect(await s.errors("other.cpp")).toEqual([]);
});
