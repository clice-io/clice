import { test } from "@clice/tools/testing";

// `clice serve --workspace` loads its workspace before any client speaks;
// the guidance that load emits reaches the client all the same.

test(async (t) => {
    const s = await t.spawn({ args: ["serve", `--workspace=${t.root}`] });
    await t.show("guidance", await s.guidance());
});
