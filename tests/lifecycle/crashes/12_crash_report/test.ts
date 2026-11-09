import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext, type WorkerFault } from "@clice/tools/testing";

// What a worker crash leaves behind, however the worker crashes: the crash
// names the request it ran, which the anomaly reaches the editor's log with;
// the worker ends as the crash would have ended it; and the crashed worker's
// own log holds a backtrace with what offline symbolization
// (scripts/symbolize.py) needs, the version and target line and each frame's
// module with its load base, from the frame the crash interrupted. The
// master's log never holds it.

const FRAME =
    /^0x[0-9a-fA-F]+(?:, .+\(0x[0-9a-fA-F]+\) \+ 0x[0-9a-fA-F]+ byte\(s\)| <unknown module>)( \(interrupted\))?$/;

/// The crash's trace, in the one log that holds one: a worker's whose name
/// starts with `prefix`.
function checkTrace(logs: Map<string, string>, prefix: string): void {
    const reports = [...logs].filter(([, text]) => text.includes("=== CRASH STACK TRACE ==="));
    expect(reports.length).toBe(1);
    const [name, report] = reports[0] ?? ["", ""];
    expect(name.startsWith(prefix)).toBe(true);
    const section = report.slice(report.indexOf("=== CRASH STACK TRACE ===")).split(/\r?\n/);
    expect(section[1]).toMatch(/^clice \S+ \S+$/);
    const frames = section.slice(2).filter((line) => FRAME.test(line));
    // The trace starts at the frame the crash interrupted, the only one marked.
    expect(frames[0]?.endsWith(" (interrupted)")).toBe(true);
    expect(frames.filter((line) => line.endsWith(" (interrupted)")).length).toBe(1);
}

/// The copy's logs, by file name.
function readLogs(t: TestContext): Map<string, string> {
    return new Map(
        t.disk.list(".clice/logs/**/*.log").map((log) => [path.basename(log), t.disk.read(log)]),
    );
}

const KINDS: readonly { name: string; kind?: NonNullable<WorkerFault["kind"]> }[] = [
    { name: "trap" },
    { name: "null write", kind: "segv" },
    { name: "abort", kind: "abort" },
    { name: "fatal error", kind: "fatal" },
    { name: "stack overflow", kind: "overflow" },
    { name: "other thread", kind: "thread" },
];

for (const { name, kind } of KINDS) {
    test(name, async (t) => {
        const s = await t.start({
            crash: { request: "hover", file: "main.cpp", ...(kind === undefined ? {} : { kind }) },
        });
        s.open("main.cpp");
        await s.sync();
        await t.show("hover", s.hover(t.at("main.cpp", "int |add")));
        await t.show("crashes", await s.crashes());
        // The editor's output channel: window/logMessage, not modelled by the
        // actions.
        expect(s.client.anomaliesInLogMessages()).toEqual(["WorkerCrash"]);
        const logs = readLogs(t);
        checkTrace(logs, "SF-");
        const master = logs.get("master.log") ?? "";
        expect(master).not.toMatch(/CRASH STACK TRACE/);
        expect(master).toMatch(
            /\[anomaly:WorkerCrash\] Worker SF-\d+ (?:killed by signal \d+ \(SIG\w+\)|terminated by exception 0x[0-9A-F]+)/,
        );
    });
}

// A unit's compile runs on a thread of its own (a stateless worker's), whose
// stack overflows as a document's request's does.
test("stack overflow while indexing", async (t) => {
    const s = await t.start({
        crash: { request: "compileUnit", file: "main.cpp", kind: "overflow" },
    });
    s.open("main.cpp");
    await t.show("sync", { failed: (await s.sync()).failed });
    await t.show("crashes", await s.crashes());
    checkTrace(readLogs(t), "SL-");
});
