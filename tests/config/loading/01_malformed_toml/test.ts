import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The project of a clice.toml that does not parse serves nothing: the
// undeclared name in main.cpp goes unreported. The server's log says why.
test("nothing is served", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await t.show("guidance", await s.guidance());
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await s.stop();
    const logs = t.disk.list(".clice/logs/*/master.log");
    expect(logs).toHaveLength(1);
    expect(t.disk.read(logs[0] ?? "")).toContain("clice.toml");
});
