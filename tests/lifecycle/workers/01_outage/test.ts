import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Requests ride out a window without a worker, the backoff a worker that
// keeps dying earns, instead of answering empty, and the file never loses
// its diagnostics. A hover only a worker can answer: the index answers none
// for a compiled document, nor does what its compile kept.

test(async (t) => {
    const s = await t.start({ killWorkers: true });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    expect(await s.killWorkers("stateful")).toBe(1);
    for (let kill = 2; kill <= 3; kill++) {
        // The worker is back once it compiles the document again.
        const park = await s.park("compile", "main.cpp", { at: "worker" });
        const hover = s.hover(t.at("main.cpp", "int x = |add(1, 2);"));
        await park.reached();
        await park.release();
        await hover;
        expect(await s.killWorkers("stateful")).toBe(1);
    }
    await t.show("hints", s.inlayHints("main.cpp"));
    await t.show("pushes", s.pushes("main.cpp"));
});
