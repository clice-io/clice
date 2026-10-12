import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

const HEADER_V1 = `#define VALUE 1
#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;
const HEADER_V2 = HEADER_V1.replace("VALUE 1", "VALUE 2").replace("TARGET alpha", "TARGET beta");

/// A rewrite below must read as one: the header is older than any.
function aged(t: TestContext): void {
    t.disk.touch("header.h", new Date("2020-01-01T00:00:00Z"));
}

/// Where closed.cpp's TARGET() call lands, alpha or beta.
async function targets(t: TestContext, s: Serve, label: string): Promise<void> {
    await t.show(label, {
        alpha: s.references(t.at("header.h", "inline int |alpha")),
        beta: s.references(t.at("header.h", "inline int |beta")),
    });
}

test("checkout under an open header", async (t) => {
    aged(t);
    const s = await t.start();
    await s.indexed();
    await targets(t, s, "v1");
    s.open("header.h");
    expect(await s.poll("workspace")).toBe(0);
    // The editor reloads a clean buffer after a checkout: a change, no save.
    // The buffer shadows the disk for the header's own compile only, so the
    // closed includer sees the checkout while the header stays open.
    t.disk.write("header.h", HEADER_V2);
    s.edit("header.h", { text: HEADER_V2 });
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    await targets(t, s, "v2");
});

test("rewrite before first tick reported", async (t) => {
    aged(t);
    const s = await t.start();
    await s.indexed();
    await targets(t, s, "v1");
    // No seeding tick: the first one judges the header against the bytes
    // the startup scan read.
    t.disk.write("header.h", HEADER_V2);
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    await targets(t, s, "v2");
});

test("touch emits no events", async (t) => {
    aged(t);
    const s = await t.start();
    await s.sync();
    expect(await s.poll("workspace")).toBe(0);
    // A new time, the same bytes: the content check stays silent.
    t.disk.write("header.h", HEADER_V1);
    expect(await s.poll("workspace")).toBe(0);
});

test("delete while open reported", async (t) => {
    const s = await t.start();
    // A buffer shadows the disk for its own file's compile only: the
    // removal is the includer's news while the header is still open.
    // Settled first: an index run looking at the header after the removal
    // would report it before the poll does.
    s.open("header.h", { pull: false });
    await s.sync();
    expect(await s.poll("workspace")).toBe(0);
    t.disk.rm("header.h");
    expect(await s.poll("workspace")).toBe(1);
    s.close("header.h");
    // Reported once.
    expect(await s.poll("workspace")).toBe(0);
});

test("background ticks see a rewrite", async (t) => {
    aged(t);
    const s = await t.start({ config: { tracker: { workspace_poll_seconds: 1 } } });
    await s.indexed();
    await targets(t, s, "v1");
    // No poll and no save, and the index answers without a look at the
    // disk: only a tick of the loop sees the rewrite.
    const seen = await s.watch("diskChange");
    t.disk.write("header.h", HEADER_V2);
    await seen.reached();
    await s.indexed();
    await targets(t, s, "v2");
});

test("deleted header reaches open includer", async (t) => {
    // A branch switch takes the header away and brings it back.
    const s = await t.start();
    expect(await s.errors("closed.cpp")).toEqual([]);
    t.disk.rm("header.h");
    expect(await s.poll("workspace")).toBe(1);
    await t.show("deleted", s.errors("closed.cpp"));
    t.disk.write("header.h", HEADER_V1);
    expect(await s.poll("workspace")).toBe(1);
    expect(await s.errors("closed.cpp")).toEqual([]);
});
