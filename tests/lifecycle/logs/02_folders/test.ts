import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// One server serves both folders and writes one log, in the logging
// directory of the project it opened first; the other project's logging
// directory links to it. Its lines name a file by its folder and its path
// there.
test(async (t) => {
    const s = await t.start({ folders: ["alpha", "beta"] });
    s.open("alpha/main.cpp");
    s.open("beta/main.cpp");
    await s.sync();
    await s.stop();
    const logs = t.disk.list(".clice/logs/*/master.log");
    expect(logs).toHaveLength(1);
    const session = path.basename(path.dirname(logs[0] ?? ""));
    expect(fs.readdirSync(path.join(t.root, "beta/.clice/logs"))).toEqual([session]);
    expect(fs.realpathSync(path.join(t.root, "beta/.clice/logs", session))).toBe(
        fs.realpathSync(path.join(t.root, ".clice/logs", session)),
    );
    const text = t.disk.read(logs[0] ?? "");
    for (const file of ["alpha/main.cpp", "beta/main.cpp"]) {
        expect(text).toContain(`Command of ${file}: from its database entry`);
    }
});
