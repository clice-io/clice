import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The rules come from the editor's settings alone: no configuration file.
const config = {
    default_configuration: "debug",
    rules: [
        { configuration: "debug", append: ["-DDEBUG"] },
        { configuration: "release", append: ["-DRELEASE"] },
    ],
};

// Selecting another configuration restarts the server on it.
test("the editor restarts", async (t) => {
    const s = await t.start({ config });
    expect(await s.switchConfiguration("release")).toEqual({ success: true, reason: null });
    await t.show("asked", await s.restarted());
    await t.show("menu", s.configurations());
});

// A client that cannot restart the server is told, by a message: there is
// no file to say it on.
test("a client without restarts", async (t) => {
    const s = await t.start({ config, capabilities: {} });
    expect(await s.switchConfiguration("release")).toEqual({ success: true, reason: null });
    await t.show("guidance", await s.guidance());
    await t.show("asked", s.restartRequests());
});

// Selecting the active one changes nothing.
test("the active one", async (t) => {
    const s = await t.start({ config });
    expect(await s.switchConfiguration("debug")).toEqual({ success: true, reason: null });
    await s.sync();
    await t.show("asked", s.restartRequests());
});

// With a configuration file, a client that cannot restart the server sees
// why it should on the file, until the active configuration is selected
// again.
test("a client without restarts, beside clice.toml", async (t) => {
    t.disk.write("clice.toml", "[project]\nenable_indexing = false\n");
    const s = await t.start({ config, capabilities: {} });
    expect(await s.switchConfiguration("release")).toEqual({ success: true, reason: null });
    await t.show("release", s.pushes("clice.toml"));
    expect(await s.switchConfiguration("debug")).toEqual({ success: true, reason: null });
    await t.show("debug again", s.pushes("clice.toml"));
});
