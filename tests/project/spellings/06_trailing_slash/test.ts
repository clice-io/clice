import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("folder spelled with a trailing slash", async (t) => {
    const s = await t.start({ folders: ["./"] });
    expect(await s.errors("main.cpp")).toEqual([]);
    // Rendering normalizes paths: the spelling is compared raw.
    const links = await s.request<{ target: string }[]>("textDocument/documentLink", "main.cpp");
    expect(links.map(({ target }) => s.spelling(target))).toEqual(["inc/h.h"]);
    const call = t.at("main.cpp", "return |f()");
    const targets = await s.request<{ uri: string }[]>("textDocument/definition", call);
    expect(targets.map(({ uri }) => s.spelling(uri))).toEqual(["inc/h.h"]);
});
