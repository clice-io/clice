import { test } from "@clice/tools/testing";
import { expect } from "vitest";

// Counterparts compile nothing of their own: before the index only names
// pair files, once it has run the definitions rank them, and an open
// buffer's definitions count over its file on disk.

test("before the index", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("include/codec.h", s.counterparts("include/codec.h"));
    await t.show("src/codec.cpp", s.counterparts("src/codec.cpp"));
});

test("indexed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("include/codec.h", s.counterparts("include/codec.h"));
    await t.show("src/decode.cpp", s.counterparts("src/decode.cpp"));
});

test("edited buffer", async (t) => {
    const s = await t.start();
    await s.indexed();
    s.open("src/decode.cpp");
    s.edit("src/decode.cpp", {
        before: "int decode(",
        insert: "int encode(int value) {\n    return value + 2;\n}\n\n",
    });
    expect(await s.errors("src/decode.cpp")).toEqual([]);
    await t.show("include/codec.h", s.counterparts("include/codec.h"));
});

test("nothing to pair", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("src/inline.h", s.counterparts("src/inline.h"));
    await t.show("src/missing.h", s.counterparts("src/missing.h"));
});
