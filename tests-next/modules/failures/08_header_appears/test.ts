import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A header appearing in a search directory that was missing at the build
// is no input the failed build recorded: saving an importer retries it,
// through the module in between.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.errors("main.cpp"));
    t.disk.write("gen/generated.h", "#pragma once\ninline int generated() {\n    return 1;\n}\n");
    s.save("main.cpp");
    expect(await s.errors("main.cpp")).toEqual([]);
});
