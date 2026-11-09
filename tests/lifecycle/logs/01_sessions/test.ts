import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

const LOGS = ".clice/logs";

/// A session directory's name: when it started, then its process.
function session(minute: number, pid: number): string {
    return `2020-01-01_00-${String(minute).padStart(2, "0")}-00_${String(pid)}`;
}

/// The entries of the logging directory, sorted.
function sessions(t: TestContext): string[] {
    return fs.readdirSync(path.join(t.root, LOGS)).sort();
}

// A start keeps the 19 latest sessions beside its own, and every session
// whose process still runs.
test("old sessions go", async (t) => {
    // Process ids past any system's limit: those processes are gone.
    const ended = Array.from({ length: 25 }, (_, minute) => session(minute + 10, 999_999_000));
    const running = `2019-01-01_00-00-00_${String(process.pid)}`;
    for (const name of [...ended, running]) {
        t.disk.write({ [`${LOGS}/${name}/index.log`]: "" });
    }
    await t.cli("index");
    const left = sessions(t);
    expect(
        left.filter((name) => !name.startsWith("2019-") && !name.startsWith("2020-")),
    ).toHaveLength(1);
    expect(left.filter((name) => name.startsWith("2019-") || name.startsWith("2020-"))).toEqual([
        running,
        ...ended.slice(-19),
    ]);
});

// A full log file moves to <name>.1.log, replacing the one there, and the
// new one starts by saying where the lines before it are.
test("a full log file moves aside", async (t) => {
    await t.cli("index", { env: { CLICE_TEST_LOG_FILE_BYTES: "1000" } });
    const [started] = sessions(t);
    const log = `${LOGS}/${started ?? ""}/index.log`;
    expect(t.disk.list(`${LOGS}/*/index*.log`)).toEqual([
        `${LOGS}/${started ?? ""}/index.1.log`,
        log,
    ]);
    expect(t.disk.read(log).split("\n")[0]).toBe("[logging] the lines before are in index.1.log");
});

/// A log line: date and time, level, thread off the main one, source line.
const LINE =
    /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[(?:trace|debug|info|warning|error|fatal)\] (?:\[thread \d+\] )?[\w.-]+:\d+ \S/;

// Every line of the server's log has the same shape; paths under the
// project are relative to it, which a line naming the project still names
// in full, and a file's command is logged once while it stays the same.
test("the server's lines", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.sync();
    for (const [from, to] of [
        ["42", "41"],
        ["41", "42"],
    ]) {
        s.edit("main.cpp", { replace: `return ${from};`, with: `return ${to};` });
        await s.sync();
    }
    await s.stop();
    const [log] = t.disk.list(`${LOGS}/*/master.log`);
    const lines = t.disk
        .read(log ?? "")
        .split(/\r?\n/)
        .filter((line) => line !== "");
    expect(lines.filter((line) => !LINE.test(line))).toEqual([]);
    const under = [t.root, fs.realpathSync.native(t.root)].flatMap((root) => [
        `${root}/`,
        `${root}${path.sep}`,
    ]);
    expect(lines.filter((line) => under.some((prefix) => line.includes(prefix)))).toEqual([]);
    expect(lines.filter((line) => line.includes("] resolve.cpp:"))).toEqual([
        expect.stringContaining("Command of main.cpp: from its database entry"),
    ]);
});
