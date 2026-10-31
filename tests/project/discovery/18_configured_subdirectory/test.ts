import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("clice index leaves sub out", async (t) => {
    await t.cli("index --jobs 1");
});

// With both folders open each unit indexes once, in its own project.
test("each unit indexes once", async (t) => {
    const s = await t.start({ folders: [".", "sub"] });
    await s.indexed();
    expect((await s.counts()).index).toBe(2);
});

// With the root open alone, a file of sub starts sub's project, which
// compiles it under sub's rules.
test("sub serves its files", async (t) => {
    const s = await t.start();
    expect(await s.errors("sub/a.cpp")).toEqual([]);
});

// main.cpp includes sub's header, which sub formats under its own rules.
test("clice format leaves sub alone", async (t) => {
    await t.cli("format");
});

// A configuration file appearing or vanishing where the root's discovery
// looks changes what the root loads: the server asks for a restart.
test("sub becomes a project", async (t) => {
    t.disk.rm("sub/clice.toml");
    const s = await t.start();
    await s.sync();
    t.disk.write("sub/clice.toml", '[[rules]]\nappend = ["-DIN_SUB"]\n');
    await t.show("asked", await s.restarted());
});

test("sub stops being a project", async (t) => {
    const s = await t.start();
    await s.sync();
    t.disk.rm("sub/clice.toml");
    await t.show("asked", await s.restarted());
});

test("a directory without a database becomes a project", async (t) => {
    const s = await t.start();
    await s.sync();
    t.disk.write("other/clice.toml", "");
    await t.show("asked", await s.restarted());
});

// A saved source the build does not declare is a provisional member: a
// configuration file appearing above it makes it another project's.
test("a project appears above a provisional member", async (t) => {
    const s = await t.start();
    s.open("scratch/pkg/new.cpp");
    s.save("scratch/pkg/new.cpp");
    await s.sync();
    t.disk.write("scratch/pkg/clice.toml", "");
    await t.show("asked", await s.restarted());
});
