import { test } from "@clice/tools/testing";

// A crash takes along the work of every document its worker hosts: that
// work is sent again, and only the document that crashed is blamed.

test(async (t) => {
    const s = await t.start({
        config: { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } },
        crash: { request: "compile", file: "poison.cpp", pragma: true },
        // One thread to compile on: the healthy compile queues behind the
        // poisoned one and is in flight when the worker dies.
        env: { UV_THREADPOOL_SIZE: "1" },
    });
    await t.show("healthy", s.diagnostics("healthy.cpp"));
    for (const round of [1, 2, 3]) {
        const park = await s.park("compile", "poison.cpp", { at: "worker" });
        if (round === 1) {
            s.open("poison.cpp");
        } else {
            s.save("poison.cpp");
        }
        const poisoned = s.hover(t.at("poison.cpp", "int |add"));
        await park.reached();
        s.edit("healthy.cpp", { after: "}\n", insert: `// round ${String(round)}\n` });
        const healthy = s.hover(t.at("healthy.cpp", "int |add"));
        await park.release();
        await t.show(`round ${String(round)}`, { poisoned, healthy });
        await t.show(`crashes after round ${String(round)}`, await s.crashes());
    }
    await t.show("healthy pushes", s.pushes("healthy.cpp"));
    await t.show("poison pushes", s.pushes("poison.cpp"));
    await t.show("healthy compiles", (await s.counts()).files["healthy.cpp"]?.compile ?? 0);
});
