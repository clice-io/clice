import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext, type WorkerFault } from "@clice/tools/testing";

// What a worker crash leaves behind, however the worker crashes: the crash
// names the request it ran, which the anomaly reaches the editor's log with;
// the worker ends as the crash would have ended it; and the crashed worker's
// own log holds a backtrace with what offline symbolization
// (scripts/symbolize.py) needs, the version and target line and each frame's
// module with its load base, from the frame the crash interrupted, which
// the master's log quotes so that log alone tells how the worker died.

const FRAME =
    /^0x[0-9a-fA-F]+(?:, .+\(0x[0-9a-fA-F]+\) \+ 0x[0-9a-fA-F]+ byte\(s\)| <unknown module>)( \(interrupted\))?$/;

/// The crash's trace, in the one worker log that holds one: a worker's whose
/// name starts with `prefix`. Returns the worker's name and the trace's
/// lines from the crash's line on.
function checkTrace(
    logs: Map<string, string>,
    prefix: string,
): { worker: string; trace: string[] } {
    const reports = [...logs].filter(
        ([name, text]) => name !== "master.log" && text.includes("=== CRASH STACK TRACE ==="),
    );
    expect(reports.length).toBe(1);
    const [name, report] = reports[0] ?? ["", ""];
    expect(name.startsWith(prefix)).toBe(true);
    const header = report.indexOf("=== CRASH STACK TRACE ===");
    const section = report.slice(header).split(/\r?\n/);
    expect(section[1]).toMatch(
        /^clice \S+ \S+( https:\/\/github\.com\/clice-io\/clice\/tree\/[0-9a-f]{40})?$/,
    );
    const frames = section.slice(2).filter((line) => FRAME.test(line));
    // The trace starts at the frame the crash interrupted, the only one marked.
    expect(frames[0]?.endsWith(" (interrupted)")).toBe(true);
    expect(frames.filter((line) => line.endsWith(" (interrupted)")).length).toBe(1);
    const crashLine = report.lastIndexOf("\n", header - 2) + 1;
    return {
        worker: path.basename(name, ".log"),
        trace: report.slice(crashLine).trimEnd().split(/\r?\n/),
    };
}

/// What the master's log quotes of `worker`'s stack trace, unindented.
function quoted(master: string, worker: string): string[] {
    const lines = master.split(/\r?\n/);
    const quote: string[] = [];
    for (const line of lines.slice(
        lines.findIndex((line) => line.endsWith(`] ${worker}'s stack trace:`)) + 1,
    )) {
        if (!line.startsWith("    ")) {
            break;
        }
        quote.push(line.slice(4));
    }
    return quote;
}

/// The anomalies the master's log reports, each from its tag on.
function anomalies(master: string): string[] {
    return master.split(/\r?\n/).flatMap((line) => {
        const tag = line.indexOf("[anomaly:");
        return tag < 0 ? [] : [line.slice(tag)];
    });
}

/// The copy's logs, by file name.
function readLogs(t: TestContext): Map<string, string> {
    return new Map(
        t.disk.list(".clice/logs/**/*.log").map((log) => [path.basename(log), t.disk.read(log)]),
    );
}

/// How the master words the worker's death: on Windows a fault ends it by
/// the exception, and abort() with 3.
function death(kind: WorkerFault["kind"]): string {
    if (process.platform !== "win32") {
        return String.raw`killed by signal \d+ \(SIG\w+\)`;
    }
    return kind === "abort" || kind === "fatal"
        ? String.raw`exited with code 3(?!\d)`
        : "terminated by exception 0x[0-9A-F]+";
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
        const { worker, trace } = checkTrace(logs, "SF-");
        const master = logs.get("master.log") ?? "";
        // The quote is the trace from its start, cut short past 16 KB, and
        // holds the frame the crash interrupted.
        const quote = quoted(master, worker);
        expect(quote).toEqual(trace.slice(0, quote.length));
        expect(quote.filter((line) => line.endsWith(" (interrupted)")).length).toBe(1);
        await t.show("anomaly", anomalies(master));
        // How the worker died, which the output elides, has its platform's
        // form.
        expect(
            anomalies(master).map((line) =>
                new RegExp(
                    String.raw`^\[anomaly:WorkerCrash\] Worker ${worker} ${death(kind)}$`,
                ).test(line),
            ),
        ).toEqual([true]);
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
