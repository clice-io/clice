import { expect } from "vitest";
import { test } from "@clice/tools/testing";

/// The log's lines at warning level and above, their time and source cut
/// off.
function warnings(text: string): string[] {
    return text.split(/\r?\n/).flatMap((line) => {
        const parsed =
            /^\[[^\]]*\] \[(warning|error|fatal)\] (?:\[thread \d+\] )?\[[\w.-]+:\d+\] (.*)$/.exec(
                line,
            );
        return parsed === null ? [] : [`${parsed[1] ?? ""}: ${parsed[2] ?? ""}`];
    });
}

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
    await t.show("log", warnings(t.disk.read(logs[0] ?? "")));
});
