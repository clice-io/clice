import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// config.h compiles on its own, where FAST is undefined and the include of
// fast.h is inactive code: fast_host.cpp's context reads it. That code
// holds no index entries, yet its include links.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("config.h")).toEqual([]);
    await t.show("config.h", {
        context: s.currentContext("config.h"),
        links: s.documentLinks("config.h"),
    });
});
