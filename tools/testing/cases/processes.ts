/// The processes one test started, however it started them: its servers,
/// its commands and those its script spawns itself — every child spawned
/// in the test's async context. Its teardown ends the ones still running
/// before it removes its copy, which Windows keeps while a process holds it.

import { AsyncLocalStorage } from "node:async_hooks";
import type { ChildProcess } from "node:child_process";
import { subscribe } from "node:diagnostics_channel";
import { once } from "node:events";

const owner = new AsyncLocalStorage<ChildProcess[]>();

subscribe("child_process", (message) => {
    owner.getStore()?.push((message as { process: ChildProcess }).process);
});

export class Processes {
    private readonly started: ChildProcess[] = [];

    /// Run `body`, every process spawned in it this test's.
    run<T>(body: () => Promise<T>): Promise<T> {
        return owner.run(this.started, body);
    }

    /// Kill the ones still running, and wait until they exited.
    async end(): Promise<void> {
        const running = this.started.filter(
            (child) =>
                child.pid !== undefined && child.exitCode === null && child.signalCode === null,
        );
        await Promise.all(
            running.map(async (child) => {
                const exited = once(child, "exit");
                child.kill("SIGKILL");
                await exited;
            }),
        );
    }
}
