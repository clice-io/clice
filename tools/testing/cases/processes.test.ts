import { spawn, type ChildProcess } from "node:child_process";
import { once } from "node:events";
import { describe, expect, test } from "vitest";
import { Processes } from "./processes.ts";

const idle = (): ChildProcess =>
    spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], { stdio: "ignore" });

describe("Processes", () => {
    test("the end kills what the test spawned, however deep, and nothing else", async () => {
        const processes = new Processes();
        const other = idle();
        const spawned: ChildProcess[] = [];
        let done: ChildProcess | undefined;
        await processes.run(async () => {
            spawned.push(idle());
            await new Promise((resolve) => setImmediate(resolve));
            await Promise.all(
                [1, 2].map(async () => {
                    await Promise.resolve();
                    spawned.push(idle());
                }),
            );
            done = spawn(process.execPath, ["-e", ""], { stdio: "ignore" });
            await once(done, "exit");
        });
        await processes.end();
        try {
            expect(spawned.map((child) => child.signalCode)).toEqual([
                "SIGKILL",
                "SIGKILL",
                "SIGKILL",
            ]);
            expect(done?.exitCode).toBe(0);
            expect(other.exitCode).toBeNull();
            expect(other.signalCode).toBeNull();
        } finally {
            other.kill("SIGKILL");
        }
    });

    test("what a test starts once it ended dies as it starts", async () => {
        const processes = new Processes();
        let resume = (): void => undefined;
        const resumed = new Promise<void>((resolve) => (resume = resolve));
        let late: ChildProcess | undefined;
        const script = processes.run(async () => {
            await resumed;
            late = idle();
            await once(late, "exit");
        });
        await processes.end();
        resume();
        await script;
        expect(late?.signalCode).toBe("SIGKILL");
    });
});
