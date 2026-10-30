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
