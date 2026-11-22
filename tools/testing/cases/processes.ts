/// The processes one test started, however it started them: its servers,
/// its commands and those its script spawns itself — every child spawned
/// in the test's async context. Its teardown ends the ones still running
/// before it removes its copy, which Windows keeps while a process holds it.

import { AsyncLocalStorage } from "node:async_hooks";
import type { ChildProcess } from "node:child_process";
import { subscribe } from "node:diagnostics_channel";
import { once } from "node:events";

interface Owner {
    started: ChildProcess[];
    ended: boolean;
}

const owner = new AsyncLocalStorage<Owner>();

subscribe("child_process", (message) => {
    const test = owner.getStore();
    if (test === undefined) {
        return;
    }
    const child = (message as { process: ChildProcess }).process;
    test.started.push(child);
    // A script past its deadline runs on once the kills end what it waited
    // for: what it starts then dies at once.
    if (test.ended) {
        child.once("spawn", () => child.kill("SIGKILL"));
    }
});

export class Processes {
    private readonly owner: Owner = { started: [], ended: false };

    /// Run `body`, every process spawned in it this test's.
    run<T>(body: () => Promise<T>): Promise<T> {
        return owner.run(this.owner, body);
    }

    /// Kill the ones still running, and wait until they exited; one the
    /// test starts after is killed as it starts.
    async end(): Promise<void> {
        this.owner.ended = true;
        const running = this.owner.started.filter(
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
