import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test } from "vitest";
import {
    logFaults,
    processGateFailures,
    serverStderrExcerpt,
    sessionLogs,
    sessionsIn,
    sessionsUnder,
    type LogFault,
} from "./process_gate.ts";

test("process gate accepts a clean exit with complete stderr", () => {
    expect(
        processGateFailures({
            exitCode: 0,
            signalCode: null,
            stderrText: "",
            stderrComplete: true,
            stderrDrainedFromStart: true,
        }),
    ).toEqual([]);
});

test("process gate rejects every incomplete or unsafe process outcome", () => {
    expect(
        processGateFailures({
            exitCode: null,
            signalCode: "SIGABRT",
            stderrText:
                "[logging] dropped 3 stderr line(s): client not draining\n" +
                "runtime error: invalid value",
            stderrComplete: false,
            stderrFailure: "timed out",
            stderrDrainedFromStart: true,
        }),
    ).toEqual([
        "server exited from signal SIGABRT",
        "stderr pump did not complete: timed out",
        "stderr mirror shed lines despite a draining client",
        "server stderr contains sanitizer/runtime error output",
    ]);
});

test("a sanitizer's report fails the gate, a file named after one does not", () => {
    const gate = (stderrText: string) =>
        processGateFailures({
            exitCode: 0,
            signalCode: null,
            stderrText,
            stderrComplete: true,
            stderrDrainedFromStart: true,
        });
    expect(
        gate(
            "[info] [pump.cpp:353] [3/9] Indexing /llvm/lib/Transforms/Instrumentation/AddressSanitizer.cpp\n" +
                "[info] [pch.cpp:152] file=/llvm/lib/Transforms/Instrumentation/HWAddressSanitizer.cpp\n" +
                "[warn] [indexer.cpp:88] no target for AddressSanitizer::instrumentFunction\n",
        ),
    ).toEqual([]);
    for (const report of [
        "==4242==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010",
        "==4242==ERROR: HWAddressSanitizer: tag-mismatch on address 0x0042",
        "WARNING: ThreadSanitizer: data race (pid=4242)",
        "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior a.cpp:3:5",
        "a.cpp:3:5: runtime error: signed integer overflow",
        "AddressSanitizer:DEADLYSIGNAL",
        "==4242==LeakSanitizer has encountered a fatal error.",
    ]) {
        expect(gate(report)).toEqual(["server stderr contains sanitizer/runtime error output"]);
    }
});

test("stderr excerpt keeps the runtime's abort messages", () => {
    const aborts = [
        "Fatal glibc error: malloc.c:2601 (sysmalloc): assertion failed: (old_top == initial_top (av) && old_size == 0)",
        "*** stack smashing detected ***: terminated",
        "[stateless-0] free(): invalid pointer",
        "double free or corruption (out)",
        "clice: llvm/lib/Support/APInt.cpp:281: void llvm::APInt::foo(): Assertion `BitWidth == RHS.BitWidth' failed.",
        "Assertion failed: (index < size), function at, file vector, line 1426.",
        "libc++abi: terminating due to uncaught exception of type std::bad_alloc",
        "terminate called after throwing an instance of 'std::length_error'",
        "LLVM ERROR: out of memory",
    ];
    const stderr = ["[info] Server ready", ...aborts, "[debug] tick", "PLEASE submit a bug report"];
    expect(serverStderrExcerpt(stderr.join("\n")).split("\n")).toEqual(aborts);
});

describe("logFaults", () => {
    // Lines as clice writes them: a worker that crashed in a request, as its
    // supervisor tells it and its own log records it.
    const hoverCrash = [
        "[2026-11-12 16:24:41.812] [info] [compile.cpp:553] Compiled main.cpp (version 0) in 6 ms: 0 diagnostics",
        "[2026-11-12 16:24:41.815] [warning] [pool.cpp:35] [SF-0] clice worker crashed in: clice/worker/hover main.cpp",
        "[2026-11-12 16:24:41.933] [error] [pool.cpp:541] The last lines SF-0 logged before it died:",
        "    [2026-11-12 16:24:41.785] [info] [worker.cc:54] Worker SF-0 (stateful) starting, pid 54869",
        "    [2026-11-12 16:24:41.811] [info] [thread 2] [stateful.cpp:320] Compiled main.cpp in 3 ms: 0 diagnostics",
        "[2026-11-12 16:24:41.933] [info] [pool.cpp:544] SF-0's stack trace:",
        "    [2026-11-12 16:24:41.815] [fatal] clice crashed",
        "    === CRASH STACK TRACE ===",
        "[2026-11-12 16:24:41.933] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SF-0 killed by signal 4 (SIGILL)",
        "[2026-11-12 16:24:41.934] [info] [pool.cpp:664] Worker SF-0 started again",
        "[2026-11-12 16:24:41.935] [warning] [diagnostics.cpp:37] main.cpp crashed a worker: clice's worker crashed while computing hover for this file (killed by signal 4 (SIGILL)).",
    ];
    const ownLog = [
        "[2026-11-12 16:24:41.785] [info] [worker.cc:54] Worker SF-0 (stateful) starting, pid 54869",
        "[2026-11-12 16:24:41.815] [fatal] clice crashed",
        "=== CRASH STACK TRACE ===",
        "clice 0.1.0+g4d17dea5 x86_64-unknown-linux-gnu https://github.com/clice-io/clice/tree/4d17dea5b74e3917136a28a41d2cf927ca74d05e",
    ];
    const lines = (file: string, texts: readonly string[]) => texts.map((text) => ({ file, text }));
    const kinds = (faults: LogFault[]) =>
        faults.map((fault) =>
            fault.kind === "crash"
                ? [fault.kind, fault.worker, fault.request, fault.file]
                : fault.kind === "anomaly"
                  ? [fault.kind, fault.id]
                  : [fault.kind, fault.line.text],
        );

    test("a worker's death is one crash, with the request it named", () => {
        const faults = logFaults([
            ...lines("/s/master.log", hoverCrash),
            ...lines("/s/SF-0.log", ownLog),
        ]);
        expect(kinds(faults)).toEqual([["crash", "SF-0", "hover", "main.cpp"]]);
        expect(faults[0]?.line.text).toContain("[anomaly:WorkerCrash]");
    });

    test("a command's terminal tells a crash as its log does", () => {
        const stderr = [
            "warning: [SL-0] clice worker crashed in: clice/worker/compileUnit poison.cpp",
            "error: The last lines SL-0 logged before it died:",
            "    [2026-11-12 16:25:00.745] [fatal] clice crashed",
            "error: [anomaly:WorkerCrash] Worker SL-0 killed by signal 4 (SIGILL)",
            "warning: Lint gave up on poison.cpp: it crashed the worker: killed by signal 4 (SIGILL)",
        ];
        expect(kinds(logFaults(lines("clice lint (stderr)", stderr)))).toEqual([
            ["crash", "SL-0", "compileUnit", "poison.cpp"],
        ]);
    });

    test("a death naming no request, on any system, is a crash of its worker", () => {
        const faults = logFaults(
            lines("/s/master.log", [
                "[2026-11-12 16:30:02.101] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SL-1 killed by signal 9 (SIGKILL)",
                "[2026-11-12 16:30:02.101] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SF-0 terminated by exception 0xC0000005 (STATUS_ACCESS_VIOLATION)",
                "[2026-11-12 16:30:02.101] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SF-1 exited with code 3",
            ]),
        );
        expect(kinds(faults)).toEqual([
            ["crash", "SL-1", undefined, undefined],
            ["crash", "SF-0", undefined, undefined],
            ["crash", "SF-1", undefined, undefined],
        ]);
    });

    test("a worker killed for running too long died in the requests it ran", () => {
        const faults = logFaults(
            lines("/s/master.log", [
                "[2026-11-12 16:24:29.861] [warning] [pool.cpp:839] Worker SF-0 ran clice/worker/compile main.cpp: killed after running for over 2 seconds",
                "[2026-11-12 16:24:29.861] [warning] [pool.cpp:839] Worker SF-0 ran clice/worker/hover main.cpp: killed after running for over 2 seconds",
                "[2026-11-12 16:24:29.862] [error] [pool.cpp:541] The last lines SF-0 logged before it died:",
                "[2026-11-12 16:24:29.863] [error] [pool.cpp:564] [anomaly:WorkerCrash] Worker SF-0 killed by signal 9 (SIGKILL)",
            ]),
        );
        expect(kinds(faults)).toEqual([["crash", "SF-0", "compile", "main.cpp"]]);
    });

    test("a death asked from outside or the machine's memory is no crash", () => {
        const faults = logFaults(
            lines("/s/master.log", [
                "[2026-11-12 16:31:00.000] [warning] [pool.cpp:35] [SL-0] LLVM ERROR: out of memory",
                "[2026-11-12 16:31:00.001] [warning] [pool.cpp:35] [SL-0] clice worker crashed in: clice/worker/index big.cpp",
                "[2026-11-12 16:31:00.100] [error] [pool.cpp:541] The last lines SL-0 logged before it died:",
                "[2026-11-12 16:31:00.100] [warning] [pool.cpp:558] Worker SL-0 ran out of memory at 2048 MiB",
                "[2026-11-12 16:31:05.000] [warning] [pool.cpp:555] Worker SF-0 terminated by signal 15",
            ]),
        );
        expect(faults).toEqual([]);
    });

    test("deaths past the anomaly's report limit are told by their requests", () => {
        const crashedIn = (file: string) =>
            `[2026-11-12 16:32:00.000] [warning] [pool.cpp:35] [SF-0] clice worker crashed in: clice/worker/hover ${file}`;
        const faults = logFaults(
            lines("/s/master.log", [
                crashedIn("a.cpp"),
                "[2026-11-12 16:32:00.100] [error] [logging.cpp:597] [anomaly:WorkerCrash] report limit (8) reached, suppressing further reports",
                crashedIn("b.cpp"),
                crashedIn("c.cpp"),
            ]),
        );
        expect(kinds(faults)).toEqual([
            ["crash", "SF-0", "hover", "a.cpp"],
            ["crash", "SF-0", "hover", "b.cpp"],
            ["crash", "SF-0", "hover", "c.cpp"],
        ]);
    });

    test("past the report limit, a death's last lines tell it, named or not", () => {
        const told = (worker: string) =>
            `[2026-11-12 16:32:00.200] [error] [pool.cpp:541] The last lines ${worker} logged before it died:`;
        const faults = logFaults(
            lines("/s/master.log", [
                ...hoverCrash.slice(1, 9),
                "[2026-11-12 16:32:00.100] [error] [logging.cpp:597] [anomaly:WorkerCrash] report limit (8) reached, suppressing further reports",
                "[2026-11-12 16:32:00.150] [warning] [pool.cpp:35] [SF-0] clice worker crashed in: clice/worker/hover b.cpp",
                told("SF-0"),
                "    [2026-11-12 16:32:00.100] [info] [worker.cc:54] Worker SF-0 (stateful) starting, pid 54870",
                told("SF-0"),
                told("SL-0"),
                "[2026-11-12 16:32:00.300] [error] [pool.cpp:424] Worker SL-1 lost: broken pipe",
                told("SL-1"),
                "[2026-11-12 16:32:00.400] [warning] [pool.cpp:35] [SF-0] clice worker crashed in: clice/worker/hover c.cpp",
                told("SF-0"),
            ]),
        );
        expect(kinds(faults)).toEqual([
            ["crash", "SF-0", "hover", "main.cpp"],
            ["crash", "SF-0", "hover", "b.cpp"],
            ["crash", "SF-0", undefined, undefined],
            ["crash", "SL-0", undefined, undefined],
            ["crash", "SL-1", undefined, undefined],
            ["crash", "SF-0", "hover", "c.cpp"],
        ]);
    });

    test("a failed wait no death follows is an error", () => {
        const lost =
            "[2026-11-12 16:32:00.300] [error] [pool.cpp:424] Worker SL-1 lost: broken pipe";
        expect(kinds(logFaults(lines("/s/master.log", [lost])))).toEqual([["error", lost]]);
    });

    test("a supervisor's crash header is its own crash; anomalies and errors stay apart", () => {
        const faults = logFaults([
            ...lines("/s/master.log", [
                "[2026-11-12 16:33:00.000] [error] [disk_state.cpp:364] [anomaly:StaleTrust] a.h was trusted as 12 bytes at 16:32:59 but holds 14 bytes at 16:33:00",
                "[2026-11-12 16:33:00.001] [error] [logging.cpp:484] Cannot create the log directory blocker/logs/2026-11-12_16-33-00_4242: Not a directory",
                "[2026-11-12 16:33:00.002] [warning] [lint.cc:189] a warning",
                "[2026-11-12 16:33:00.003] [fatal] clice crashed",
                "=== CRASH STACK TRACE ===",
            ]),
            ...lines("clice command (stderr)", [
                "warning: Cannot ask the compiler for its default settings: Cannot find driver: missing-clang++; clang's own driver reads its commands",
                "error: The command stays a driver command: Cannot find driver: missing-clang++",
            ]),
        ]);
        expect(kinds(faults)).toEqual([
            ["anomaly", "StaleTrust"],
            [
                "error",
                "[2026-11-12 16:33:00.001] [error] [logging.cpp:484] Cannot create the log directory blocker/logs/2026-11-12_16-33-00_4242: Not a directory",
            ],
            ["crash", undefined, undefined, undefined],
            [
                "error",
                "error: The command stays a driver command: Cannot find driver: missing-clang++",
            ],
        ]);
    });
});

describe("sessions", () => {
    test("a session is found by its process's pid wherever it logs, and read once", () => {
        const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-sessions-"));
        const session = path.join(root, "app", "logs", ".clice", "2026-11-12_16-24-41_4242");
        fs.mkdirSync(session, { recursive: true });
        fs.writeFileSync(path.join(session, "master.log"), "");
        fs.writeFileSync(path.join(session, "master.1.log"), "");
        fs.writeFileSync(path.join(session, "SF-0.log"), "");
        const other = path.join(root, "beta", ".clice", "logs");
        fs.mkdirSync(other, { recursive: true });
        fs.symlinkSync(session, path.join(other, "2026-11-12_16-24-41_4242"), "junction");
        fs.mkdirSync(path.join(root, ".clice", "logs", "2026-11-12_16-25-00_77"), {
            recursive: true,
        });

        const found = sessionsUnder(root);
        expect([...found.keys()].sort()).toEqual([4242, 77]);
        expect(found.get(4242)).toHaveLength(1);
        expect(
            sessionLogs([
                ...(found.get(4242) ?? []),
                path.join(other, "2026-11-12_16-24-41_4242"),
            ]).map((file) => path.basename(file)),
        ).toEqual(["SF-0.log", "master.1.log", "master.log"]);
        expect(sessionsIn(other).map((dir) => path.basename(dir))).toEqual([
            "2026-11-12_16-24-41_4242",
        ]);
        expect(sessionsIn(path.join(root, "nothing"))).toEqual([]);
        fs.rmSync(root, { recursive: true, force: true });
    });
});
