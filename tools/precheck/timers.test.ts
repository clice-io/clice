import { describe, expect, test } from "vitest";
import { LABELS } from "../lint/rules.ts";
import { unlabelledTimers } from "./timers.ts";

const source = (...lines: string[]): string => lines.join("\n");
const unlabelled = (...lines: string[]): string[] =>
    unlabelledTimers("src/a.cpp", source(...lines));

describe("unlabelledTimers", () => {
    test("every kind of timer needs a label", () => {
        expect(
            unlabelled(
                "co_await async::sleep(std::chrono::milliseconds(10));",
                "co_await async::with_timeout(request(), 5s);",
                "    timer->start(interval);",
                "std::this_thread::sleep_for(1s);",
                "std::this_thread::sleep_until(deadline);",
                "options.timeout = chrono::seconds(5);",
                "auto watchdog = chrono::minutes(1);",
            ),
        ).toEqual([
            "src/a.cpp:1: co_await async::sleep(std::chrono::milliseconds(10));",
            "src/a.cpp:2: co_await async::with_timeout(request(), 5s);",
            "src/a.cpp:3: timer->start(interval);",
            "src/a.cpp:4: std::this_thread::sleep_for(1s);",
            "src/a.cpp:5: std::this_thread::sleep_until(deadline);",
            "src/a.cpp:6: options.timeout = chrono::seconds(5);",
            "src/a.cpp:7: auto watchdog = chrono::minutes(1);",
        ]);
    });

    test.each(LABELS)("the label %s allows the timer below it", (label) => {
        expect(unlabelled(`// ${label} why.`, "co_await async::sleep(1s);")).toEqual([]);
    });

    test("a label anywhere in the comment block counts, after any number of slashes", () => {
        expect(
            unlabelled(
                "/// Hang detection: a worker that never answers.",
                "/// The bound is a minute.",
                "co_await async::with_timeout(request(), 1min);",
                "// The pace of the trace.",
                "// Time is the feature: a recorded pace.",
                "co_await async::sleep(pace);",
            ),
        ).toEqual([]);
    });

    test("the label goes above the first line of the statement the timer is in", () => {
        expect(
            unlabelled(
                "{",
                "    // No event: the file is read again.",
                "    auto result = co_await read(path,",
                "        async::sleep(1s));",
                "}",
            ),
        ).toEqual([]);
        expect(
            unlabelled(
                "auto result = co_await read(path,",
                "    // No event: the file is read again.",
                "    async::sleep(1s));",
            ),
        ).toEqual([]);
    });

    test("a label above an earlier statement does not count", () => {
        expect(
            unlabelled("// Hang detection: a hang.", "start();", "co_await async::sleep(1s);"),
        ).toEqual(["src/a.cpp:3: co_await async::sleep(1s);"]);
    });

    test("a statement ends before its trailing comment", () => {
        expect(
            unlabelled(
                "// Hang detection: a hang.",
                "co_await async::sleep(1s); // the first",
                "co_await async::sleep(2s);",
            ),
        ).toEqual(["src/a.cpp:3: co_await async::sleep(2s);"]);
    });

    test("a label without its why, or in prose, does not count", () => {
        expect(
            unlabelled(
                "// Hang detection:",
                "co_await async::sleep(1s);",
                "// A hang. Hang detection: a bound.",
                "co_await async::sleep(2s);",
            ),
        ).toEqual([
            "src/a.cpp:2: co_await async::sleep(1s);",
            "src/a.cpp:4: co_await async::sleep(2s);",
        ]);
    });

    test("a zero wait and the test hook's jitter are no timers", () => {
        expect(
            unlabelled(
                "co_await async::sleep(chrono::milliseconds(0));",
                "co_await async::sleep(milliseconds(0));",
                "co_await async::sleep(test_jitter());",
                "this_thread::sleep_for(test_jitter());",
                "idle_timer->start(chrono::milliseconds(0));",
            ),
        ).toEqual([]);
        expect(
            unlabelled("co_await async::with_timeout(poll(chrono::milliseconds(0)), 5s);"),
        ).toEqual([
            "src/a.cpp:1: co_await async::with_timeout(poll(chrono::milliseconds(0)), 5s);",
        ]);
    });

    test("commented timers and other names are no timers", () => {
        expect(
            unlabelled(
                "// co_await async::sleep(1s);",
                "do_sleep_for(1s);",
                "auto read_timeout = chrono::seconds(5);",
            ),
        ).toEqual([]);
    });
});
