import { test } from "@clice/tools/testing";

// The server indexes one configuration: asked for another, it refuses
// rather than answer with the wrong build.
test(async (t) => {
    const s = await t.start({ args: ["serve", "--configuration", "debug"] });
    await s.indexed();
    await t.cli("index --configuration release");
    await t.cli("index --configuration debug");
});
