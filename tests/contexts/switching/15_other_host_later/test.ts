import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const REFRESH = "workspace/semanticTokens/refresh";
const CAPABILITIES = { workspace: { semanticTokens: { refreshSupport: true } } };

// slow_path() is code only slow_host.cpp's context compiles: the answers
// for shared.h, compiled in fast_host.cpp's, take it from the index while
// the buffer is the text the index read.

test("a host indexed later", async (t) => {
    // The other context's code shows once its host is indexed, and the
    // client is asked for what it showed before.
    t.disk.database({});
    const s = await t.start({ capabilities: CAPABILITIES });
    const slow = await s.park("index", "slow_host.cpp", { at: "reply" });
    t.disk.database();
    expect(await s.poll("cdb")).toBe(1);
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("before", s.hover(t.marker("slow")));
    // Counted without waiting for the server to go idle, which the parked
    // index keeps it from.
    const refreshes = s.client.serverRequests.filter((sent) => sent === REFRESH).length;
    await slow.release();
    await s.indexed();
    expect(await s.serverRequests(REFRESH)).toBeGreaterThan(refreshes);
    await t.show("indexed", s.hover(t.marker("slow")));
});

test("an edit until saved", async (t) => {
    const s = await t.start({ capabilities: CAPABILITIES });
    await s.indexed();
    expect(await s.errors("shared.h")).toEqual([]);
    const slow = t.at("shared.h", "inline int |slow_path");
    s.edit("shared.h", { replace: "#endif", with: "#endif // FAST" });
    await t.show("edited", s.hover(slow));
    const refreshes = await s.serverRequests(REFRESH);
    s.save("shared.h");
    await s.indexed();
    expect(await s.serverRequests(REFRESH)).toBeGreaterThan(refreshes);
    await t.show("saved", s.hover(slow));
});
