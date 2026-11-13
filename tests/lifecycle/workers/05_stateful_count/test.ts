import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The pool keeps as many stateful workers as the project asks for.
for (const count of [1, 3]) {
    test(`${String(count)} stateful workers`, async (t) => {
        const s = await t.start({ config: { project: { stateful_worker_count: count } } });
        await s.diagnostics("main.cpp");
        expect((await s.stats()).workers.stateful).toHaveLength(count);
    });
}
