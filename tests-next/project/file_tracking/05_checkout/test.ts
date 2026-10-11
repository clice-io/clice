import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const HEADER_V2 = `#define VALUE 2
#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

test("checkout updates workspace", async (t) => {
    // A rewrite below must read as one: the inputs are older than any.
    for (const file of ["header.h", "closed.cpp"]) {
        t.disk.touch(file, new Date("2020-01-01T00:00:00Z"));
    }
    const s = await t.start();
    await s.sync();
    await t.show("v1", s.errors("main.cpp"));
    await s.indexed();
    await t.show("v1 alpha", s.references(t.at("header.h", "inline int |alpha")));
    expect(await s.poll("workspace")).toBe(0);

    // A checkout rewrites files on disk; nothing is saved.
    t.disk.write("header.h", HEADER_V2);
    t.disk.edit("closed.cpp", {
        after: "TARGET(); }\n",
        insert: "int checkout_added() { return 3; }\n",
    });
    expect(await s.poll("workspace")).toBe(2);
    // The open file compiles against the new header; the closed one is
    // indexed again, against it and with its own change.
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("v2", {
        beta: s.references(t.at("header.h", "inline int |beta")),
        added: s.workspaceSymbols("checkout_added"),
    });
});
