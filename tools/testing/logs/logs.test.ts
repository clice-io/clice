import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, describe, expect, test } from "vitest";
import { gateLogs, type Declared, type Started } from "./logs.ts";

const roots: string[] = [];
afterEach(() => {
    for (const root of roots.splice(0)) {
        fs.rmSync(root, { recursive: true, force: true });
    }
});

/// A copy whose process `pid` wrote `logs` (name to lines) into a session
/// directory at `dir`, `.clice/logs` by default.
function copy(pid: number, logs: Record<string, readonly string[]>, dir = ".clice/logs"): string {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-gate-"));
    roots.push(root);
    const session = path.join(root, dir, `2026-11-12_16-24-41_${String(pid)}`);
    fs.mkdirSync(session, { recursive: true });
    for (const [name, lines] of Object.entries(logs)) {
        fs.writeFileSync(path.join(session, name), `${lines.join("\n")}\n`);
    }
    return root;
}

const SERVER: Started = { label: "server 1", pid: 4242, stderr: "" };

function crash(worker: string, request: string, file: string): string[] {
    return [
        `[2026-11-12 16:24:41.815] [warning] [pool.cpp:35] [${worker}] clice worker crashed in: clice/worker/${request} ${file}`,
        `[2026-11-12 16:24:41.933] [error] [pool.cpp:541] The last lines ${worker} logged before it died:`,
        `    [2026-11-12 16:24:41.785] [info] [worker.cc:54] Worker ${worker} (stateful) starting, pid 54869`,
        `[2026-11-12 16:24:41.933] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker ${worker} killed by signal 4 (SIGILL)`,
    ];
}

const OWN_CRASH = [
    "[2026-11-12 16:24:41.785] [info] [worker.cc:54] Worker SF-0 (stateful) starting, pid 54869",
    "[2026-11-12 16:24:41.815] [fatal] clice crashed",
    "=== CRASH STACK TRACE ===",
];

function gate(root: string, declared: Declared, started: readonly Started[] = [SERVER]): string[] {
    return gateLogs(root, started, declared, false).map((error) => (error as Error).message);
}

describe("crashes", () => {
    const hover = { crash: { request: "hover", file: "a.cpp" } };

    test("the declared crash passes", () => {
        const root = copy(4242, {
            "master.log": crash("SF-0", "hover", "a.cpp"),
            "SF-0.log": OWN_CRASH,
        });
        expect(gate(root, { servers: [hover] })).toEqual([]);
    });

    test("another crash fails", () => {
        const root = copy(4242, {
            "master.log": [...crash("SF-0", "hover", "a.cpp"), ...crash("SF-0", "hover", "b.cpp")],
        });
        expect(gate(root, { servers: [hover] })).toEqual([
            "worker crashes nothing declared:\nhover b.cpp",
        ]);
    });

    test("a declared crash that never happened fails", () => {
        const root = copy(4242, {
            "master.log": ["[2026-11-12 16:24:41.000] [info] [x.cpp:1] nothing"],
        });
        expect(
            gate(root, { servers: [hover, { crash: { request: "hover", file: "b.cpp" } }] }),
        ).toEqual(["declared crashes that never happened:\nhover a.cpp\nhover b.cpp"]);
    });

    test("a command's declared crash passes", () => {
        const root = copy(77, { "lint.log": crash("SL-0", "compileUnit", "a.cpp") });
        const lint = { label: "$ clice lint", pid: 77, stderr: "" };
        expect(gate(root, {}, [lint])).not.toEqual([]);
        const compileUnit = { crash: { request: "compileUnit", file: "a.cpp" } };
        expect(gate(root, { commands: [compileUnit] }, [lint])).toEqual([]);
    });

    test("a crash declared in a folder matches the path the log writes in it", () => {
        const root = copy(4242, { "master.log": crash("SF-0", "hover", "main.cpp") });
        expect(
            gate(root, { servers: [{ crash: { request: "hover", file: "app/main.cpp" } }] }),
        ).toEqual([]);
        expect(
            gate(root, { servers: [{ crash: { request: "hover", file: "app/other.cpp" } }] }),
        ).not.toEqual([]);
    });

    test("a kill is a death naming no request", () => {
        const root = copy(4242, {
            "master.log": [
                "[2026-11-12 16:30:02.101] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SL-1 killed by signal 9 (SIGKILL)",
            ],
        });
        expect(gate(root, { servers: [{ killed: 1 }] })).toEqual([]);
        expect(gate(root, {})).toEqual([
            "the declared kills explain 0 worker deaths naming no request; the logs have:\nmaster.log: [2026-11-12 16:30:02.101] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SL-1 killed by signal 9 (SIGKILL)",
        ]);
    });

    test("clice crashing itself fails", () => {
        const root = copy(4242, {
            "master.log": ["[2026-11-12 16:24:41.815] [fatal] clice crashed"],
        });
        expect(gate(root, { servers: [hover] })).toEqual([
            "clice crashed:\nmaster.log: [2026-11-12 16:24:41.815] [fatal] clice crashed",
            "declared crashes that never happened:\nhover a.cpp",
        ]);
    });
});

describe("anomalies and errors", () => {
    const anomaly =
        "[2026-11-12 16:33:00.000] [error] [disk_state.cpp:364] [anomaly:StaleTrust] a.h was trusted as 12 bytes but holds 14 bytes";
    const broke = "[2026-11-12 16:33:00.001] [error] [x.cpp:1] something broke";

    test("an anomaly nothing declared fails, whatever errors the test declares", () => {
        const root = copy(4242, { "master.log": [anomaly] });
        expect(gate(root, { errors: [/anomaly/] })).toEqual([
            `anomalies nothing declared:\nmaster.log: ${anomaly}`,
            "declared errors no error-level line matches:\nanomaly",
        ]);
        expect(gate(root, { servers: [{ anomalies: true }] })).toEqual([]);
    });

    test("undeclared error lines fail", () => {
        const root = copy(4242, { "master.log": [broke] });
        expect(gate(root, {})).toEqual([
            `error-level log lines the test did not declare:\nmaster.log: ${broke}`,
        ]);
        expect(gate(root, { errors: [/something broke/] })).toEqual([]);
    });

    test("a declared error no line matches fails", () => {
        const root = copy(4242, { "master.log": [broke] });
        expect(gate(root, { errors: [/something broke/, /never logged/] })).toEqual([
            "declared errors no error-level line matches:\nnever logged",
        ]);
    });
});

describe("where the logs are", () => {
    const broke = "[2026-11-12 16:33:00.001] [error] [x.cpp:1] something broke";

    test("a session is found wherever its process logged", () => {
        const root = copy(4242, { "master.log": [broke] }, "app/logs/.clice");
        expect(gate(root, {})).toEqual([
            `error-level log lines the test did not declare:\nmaster.log: ${broke}`,
        ]);
    });

    test("a process that wrote no session is judged by its stderr", () => {
        const root = copy(1, { "master.log": [broke] });
        const command = {
            label: "$ clice command convert",
            pid: 4243,
            stderr: "warning: Cannot find driver: cc\nerror: The command stays a driver command: Cannot find driver: cc\n",
        };
        expect(gate(root, {}, [command])).toEqual([
            "error-level log lines the test did not declare:\n$ clice command convert (stderr): error: The command stays a driver command: Cannot find driver: cc",
        ]);
        expect(gate(root, { errors: [/stays a driver command/] }, [command])).toEqual([]);
    });
});
