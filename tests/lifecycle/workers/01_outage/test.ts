import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Requests ride out a window without a worker, the backoff a worker that
// keeps dying earns, instead of answering empty, and the file never loses
// its diagnostics. Inlay hints only a worker can answer: the index has none.

test(async (t) => {
    const s = await t.start({ killWorkers: true });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    expect(await s.killWorkers("stateful")).toBe(1);
    for (let kill = 2; kill <= 3; kill++) {
        // The worker is back once it compiles the document again.
        const park = await s.park("compile", "main.cpp", { at: "worker" });
        const hints = s.inlayHints("main.cpp");
        await park.reached();
        await park.release();
        await hints;
        expect(await s.killWorkers("stateful")).toBe(1);
    }
    await t.show("hints", s.inlayHints("main.cpp"));
    await t.show("pushes", s.pushes("main.cpp"));
});
