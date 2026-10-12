import { expect } from "vitest";
import { HoldRequest, type BuildKind } from "@clice/tools/protocol";
import { refusal, test } from "@clice/tools/testing";

// The parks: a hold parks a build's reply in the server, a gate a
// request's work where it starts in its worker; either way the server
// counts the work as in flight, publishes nothing for it, and lets it go
// on release or shutdown.

test("sync waits for requests", async (t) => {
    const s = await t.start();
    // The pull waits for a compile no build had started when the syncs
    // arrived; neither sync waits for the other.
    s.open("main.cpp", { pull: false });
    let answered = false;
    const pulled = s.diagnostics("main.cpp").then((diagnostics) => {
        answered = true;
        return diagnostics;
    });
    const syncs = await Promise.all([s.client.sync(), s.client.sync()]);
    expect(answered).toBe(true);
    expect(syncs.map((sync) => sync.pending)).toEqual([[], []]);
    await pulled;
});

for (const at of ["reply", "worker"] as const) {
    test(`a ${at} park holds a compile`, async (t) => {
        const s = await t.start();
        await s.sync();
        const park = await s.park("compile", "main.cpp", { at });
        s.open("main.cpp", { pull: false });
        const pulled = s.diagnostics("main.cpp");
        await park.reached();
        // The editor's view of the pushes: s.pushes would wait for the
        // parked work.
        expect(s.client.publishCount(s.uri("main.cpp"))).toBe(0);
        await t.show("pending", (await s.client.sync({ deadlineMs: 1_000 })).pending);
        await park.release();
        await t.show("diagnostics", pulled);
        await t.show("compiles", (await s.counts()).files["main.cpp"]?.compile ?? 0);
    });
}

test("shutdown releases a park", async (t) => {
    const s = await t.start();
    await s.sync();
    const park = await s.park("compile", "main.cpp", { at: "reply" });
    s.open("main.cpp", { pull: false });
    const hover = s.hover(t.at("main.cpp", "int |main")).catch(() => null);
    await park.reached();
    // The exit gate fails a server that does not exit.
    await s.stop();
    await hover;
});

test("parks name work and a file", async (t) => {
    const s = await t.start();
    await t.show("refused", {
        "a hold of no build": await refusal(
            s.client.sendRequest(HoldRequest, {
                kind: "query" as BuildKind,
                uri: s.uri("main.cpp"),
            }),
        ),
        "a hold of no file": await refusal(s.client.hold("compile", "untitled:main")),
        "a gate of no request": await refusal(s.client.gate("", s.uri("main.cpp"))),
        "a release of no park": await refusal(s.client.release(7)),
    });
});
