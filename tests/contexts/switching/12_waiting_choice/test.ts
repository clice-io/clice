import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A chosen host that stops including the header leaves the choice waiting:
// the header compiles in the context clice picks, and in the chosen one again
// once the host includes it again.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("int_host.cpp")).toEqual([]);
    expect(await s.errors("real_host.cpp")).toEqual([]);
    s.open("shared.h");
    expect((await s.switchContext("shared.h", "real_host.cpp")).success).toBe(true);
    const decl = t.at("shared.h", "VALUE_TYPE |get_value");
    await t.show("chosen", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
    const host = t.disk.read("real_host.cpp");
    s.edit("real_host.cpp", { replace: '#include "shared.h"', with: "" });
    s.save("real_host.cpp");
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("waiting", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
    s.edit("real_host.cpp", { text: host });
    s.save("real_host.cpp");
    expect(await s.errors("shared.h")).toEqual([]);
    await t.show("chosen again", { context: s.currentContext("shared.h"), hover: s.hover(decl) });
});
