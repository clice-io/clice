import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A folder nested in another, listed first: the lines still name each file
// by the folders' common directory, the outer folder's files as well.
test(async (t) => {
    const s = await t.start({ folders: ["app/sub", "app"] });
    s.open("app/main.cpp");
    s.open("app/sub/main.cpp");
    await s.sync();
    await s.stop();
    const logs = t.disk.list(".clice/logs/*/master.log");
    expect(logs).toHaveLength(1);
    const text = t.disk.read(logs[0] ?? "");
    expect(text).toContain("Command of app/main.cpp:");
    expect(text).toContain("Command of app/sub/main.cpp:");
});
