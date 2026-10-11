import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const IMPL =
    "module m;\n#ifndef FROM_MODS\n#error needs the interface's flags\n#endif\nint mv() { return 1; }\n";

test("implementation unit borrows interface", async (t) => {
    const s = await t.start();
    t.disk.write("mods/m_impl.cpp", IMPL);
    // The interface next to it lends.
    expect(await s.errors("mods/m_impl.cpp")).toEqual([]);
    s.save("mods/m_impl.cpp");
    // And keeps lending once it is a member.
    s.edit("mods/m_impl.cpp", {
        after: "int mv() { return 1; }\n",
        insert: "int twice() { return mv() + mv(); }\n",
    });
    expect(await s.errors("mods/m_impl.cpp")).toEqual([]);
});
