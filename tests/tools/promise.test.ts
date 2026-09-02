/// Tests for the shared promise helpers (tools/promise.ts).

import { expect, test } from "vitest";
import { TimeoutError, withTimeout } from "@clice/tools/promise";

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
