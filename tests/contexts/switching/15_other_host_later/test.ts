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
    // index keeps it from: a refresh fast_host.cpp's index sends may come
    // before or after the count, so only the growth is certain.
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
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await t.show("saved", s.hover(slow));
});

test("hidden while read only", async (t) => {
    // Served from the index, which merges both contexts, the file compiles
    // once its other contexts are hidden.
    const s = await t.start({ config: { project: { readonly: "auto" } } });
    await s.indexed();
    s.open("shared.h", { pull: false });
    await t.show("merged", s.documentSymbols("shared.h"));
    expect((await s.showOtherContexts("shared.h", false)).success).toBe(true);
    await t.show("hidden", s.documentSymbols("shared.h"));
});

test("hidden where nothing compiles", async (t) => {
    // The index alone answers there: hiding is refused.
    const s = await t.start({ config: { project: { readonly: "on" } } });
    await s.indexed();
    s.open("shared.h", { pull: false });
    expect((await s.showOtherContexts("shared.h", false)).success).toBe(false);
    await t.show("context", s.currentContext("shared.h"));
});
