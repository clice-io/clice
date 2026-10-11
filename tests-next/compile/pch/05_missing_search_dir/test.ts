import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const GENERATED = "#pragma once\n\ninline int generated() {\n    return 1;\n}\n";

test("generated search directory", async (t) => {
    const s = await t.start();
    await t.show("missing", s.errors("main.cpp"));
    // The generator creates the search directory with the header.
    t.disk.write("gen/generated.h", GENERATED);
    await s.sync({ poll: true });
    await t.show("generated", s.errors("main.cpp"));
    // A save retries a preamble that had errors, and the rebuilt PCH
    // replaces the failed one in the cache.
    s.save("main.cpp");
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(t.disk.list(".clice/cache/*/pch/*.pch")).toHaveLength(1);
});
