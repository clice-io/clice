/// Tests for the shared promise helpers (tools/promise.ts).

import { MessageChannel } from "node:worker_threads";
import { expect, test } from "vitest";
import { TimeoutError, withTimeout } from "./promise.ts";

// Time is the feature: the timeout itself is tested.
test("timeout settles with the promise or a timeout error", async () => {
    await expect(withTimeout(Promise.resolve(7), 50)).resolves.toBe(7);

    const failure = new Error("boom");
    await expect(withTimeout(Promise.reject(failure), 50)).rejects.toBe(failure);

    const hang = new Promise<never>(() => undefined);
    const timeout = await withTimeout(hang, 5, "the hang").catch((error: unknown) => error);
    expect(timeout).toBeInstanceOf(TimeoutError);
    expect((timeout as Error).message).toBe("timed out after 5ms: the hang");
    const bare = await withTimeout(hang, 5).catch((error: unknown) => error);
    expect((bare as Error).message).toBe("timed out after 5ms");
});

// Time is the feature: the timeout itself is tested.
test("timeout lets I/O ready after a stall settle first", async () => {
    // From the check phase, the next turn runs the due timer before it
    // delivers the message.
    await new Promise((resolve) => setImmediate(resolve));
    const { port1, port2 } = new MessageChannel();
    const arrived = new Promise((resolve) => port2.once("message", resolve));
    const waiting = withTimeout(arrived, 20);
    port1.postMessage("ready");
    const end = Date.now() + 60;
    while (Date.now() < end) {
        // The stall: the deadline passes with the message already sent.
    }
    await expect(waiting).resolves.toBe("ready");
    port1.close();
});
