/// vitest binding of the session machinery (tools/client/session.ts) for
/// the integration suite: the `session` fixture ties one factory to each
/// test and runs the teardown gates when it ends.

import { test as base } from "vitest";
import type { CliceClient } from "@clice/tools/client";
import {
    cliceExecutable,
    createSessionFactory,
    type Session,
    type SessionFactory,
    type SessionOptions,
} from "@clice/tools/session";
import type { Workspace } from "@clice/tools/workspace";

export { expect } from "vitest";
export { cliceExecutable, type Session, type SessionFactory, type SessionOptions };

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

/// The zero-boilerplate form for files whose tests all target one
/// workspace — the TS equivalent of pytest's `@pytest.mark.workspace`
/// marker plus `client`/`workspace` fixtures:
///
///     const test = cliceTest("document_links");
///     test("links with pch", async ({ client, workspace }) => { ... });
///
/// Setup (spawn + initialize) and teardown (shutdown gate, anomaly gate,
/// .clice cleanup, workspace lock) are fully automatic. Tests needing
/// several servers, custom argv or a temp workspace use the `session`
/// factory from `test` instead.
export function cliceTest(name: string, options: SessionOptions = {}) {
    return test.extend<Session & { bound: Session }>({
        bound: async (
            { session }: { session: SessionFactory },
            use: (bound: Session) => Promise<void>,
        ) => {
            await use(await session(name, options));
        },
        client: async (
            { bound }: { bound: Session },
            use: (client: CliceClient) => Promise<void>,
        ) => {
            await use(bound.client);
        },
        workspace: async (
            { bound }: { bound: Session },
            use: (workspace: Workspace) => Promise<void>,
        ) => {
            await use(bound.workspace);
        },
    });
}
