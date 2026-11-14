import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const GENERATED = "#pragma once\n\ninline int generated() {\n    return 1;\n}\n";

test("generated search directory", async (t) => {
    const s = await t.start();
    await t.show("missing", s.errors("main.cpp"));
    // The generator creates the search directory with the header: the
    // place the failed lookup looked in was missing, and the header there is
    // found without a save.
    t.disk.write("gen/generated.h", GENERATED);
    await s.sync({ poll: true });
    expect(await s.errors("main.cpp")).toEqual([]);
});
