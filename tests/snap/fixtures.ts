/// vitest binding of the session machinery (tools/client/session.ts) for
/// the snap suite: the server driver replays fixtures through real servers,
/// so it needs the same `session` fixture and teardown gates as the
/// integration suite — bound independently, per suite.

import { test as base } from "vitest";
import { cliceExecutable, createSessionFactory, type SessionFactory } from "@clice/tools/session";

export { expect } from "vitest";
export { cliceExecutable };

export const test = base.extend<{ session: SessionFactory }>({
    session: async (
        { task }: { task: { result?: { errors?: unknown[] } } },
        use: (factory: SessionFactory) => Promise<void>,
    ) => {
        const handle = createSessionFactory();
        try {
            await use(handle.session);
        } finally {
            await handle.teardown((task.result?.errors?.length ?? 0) > 0);
        }
    },
});
