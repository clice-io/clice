import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, expect, test } from "vitest";
import { gateLogs } from "./gate.ts";

const roots: string[] = [];

afterEach(() => {
    for (const root of roots.splice(0)) {
        fs.rmSync(root, { recursive: true, force: true });
    }
});

const SESSION = path.join(".clice", "logs", "2026-11-13_09-00-00_4242");

/// A directory of copies, each with the master log of one session.
function copies(logs: Record<string, string[] | null>): string {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-editors-gate-"));
    roots.push(root);
    for (const [copy, lines] of Object.entries(logs)) {
        const dir = path.join(root, copy, SESSION);
        fs.mkdirSync(dir, { recursive: true });
        if (lines !== null) {
            fs.writeFileSync(path.join(dir, "master.log"), `${lines.join("\n")}\n`);
        }
    }
    return root;
}

test("clean logs pass", () => {
    expect(gateLogs(copies({ a: ["[t] [info] [x.cpp:1] compiled"] }))).toEqual([]);
});

test("an error, a fatal line and an anomaly fail, a warning does not", () => {
    const failures = gateLogs(
        copies({
            a: [
                "[t] [warning] [x.cpp:1] slow",
                "[t] [error] [x.cpp:2] broken",
                "[t] [fatal] clice crashed",
            ],
            b: ["[t] [info] [pool.cpp:3] [anomaly:StaleAnswer] told stale"],
        }),
    );
    expect(failures).toEqual([
        `a: ${path.join(SESSION, "master.log")}: [t] [error] [x.cpp:2] broken`,
        `a: ${path.join(SESSION, "master.log")}: [t] [fatal] clice crashed`,
        `b: ${path.join(SESSION, "master.log")}: [t] [info] [pool.cpp:3] [anomaly:StaleAnswer] told stale`,
    ]);
});

test("a copy no server logged under fails", () => {
    const root = copies({ a: null });
    expect(gateLogs(root)).toEqual([`a: no server logged under ${path.join(root, "a")}`]);
});
