/// vitest binding of the session machinery (tools/client/session.ts) for
/// the integration suite: the `session` fixture ties one factory to each
/// test and runs the teardown gates when it ends.

import { test as base } from "vitest";
import { Serve, type FileText, type ServeOptions } from "@clice/tools/actions";
import {
    cliceExecutable,
    createSessionFactory,
    type Session,
    type SessionFactory,
    type SessionOptions,
} from "@clice/tools/session";
import { sessionFixture } from "../session_fixture.ts";

export { expect } from "vitest";
export { at, type Loc } from "@clice/tools/actions";
export { cliceExecutable, type Session, type SessionFactory, type SessionOptions };

export const test = base.extend<{ session: SessionFactory }>({
    session: sessionFixture,
});

/// A serve case's own hang detection: its waits end on server events, so
/// only a case that hangs runs into it.
const CASE_TIMEOUT = 300_000;

type ServeBody = (context: { s: Serve }) => void | Promise<void>;

export interface ServeTest {
    (name: string, body: ServeBody): void;
    for<T>(
        cases: readonly T[],
    ): (name: string, body: (item: T, context: { s: Serve }) => void | Promise<void>) => void;
    /// The cases registered through it are skipped when `condition` holds.
    skipIf(condition: boolean): ServeTest;
}

function serveTest(project: string | null, options: ServeOptions, skip = false): ServeTest {
    const extended = base.extend<{ s: Serve }>({
        s: async ({ task }, use) => {
            const handle = createSessionFactory();
            let started = false;
            let s: Serve | undefined;
            try {
                s = await Serve.create(handle.session, project, options);
                started = true;
                await use(s);
            } finally {
                let failed = !started || (task.result?.errors?.length ?? 0) > 0;
                try {
                    await s?.finish(failed);
                } catch (error) {
                    failed = true;
                    throw error;
                } finally {
                    await handle.teardown(failed);
                }
            }
        },
    });
    const bound = extended.skipIf(skip);
    const run = (name: string, body: ServeBody): void => {
        bound(name, { timeout: CASE_TIMEOUT }, body);
    };
    run.for =
        <T>(cases: readonly T[]) =>
        (name: string, body: (item: T, context: { s: Serve }) => void | Promise<void>): void => {
            bound.for(cases)(name, { timeout: CASE_TIMEOUT }, body);
        };
    run.skipIf = (condition: boolean): ServeTest => serveTest(project, options, skip || condition);
    return run;
}

/// Cases on a copy of the sample project samples/<project>, each
/// with its own copy and server (see @clice/tools/actions):
///
///     const test = serve("shapes/headers");
///     test("deleted source withdraws its rows", async ({ s }) => { ... });
export function serve(project: string, options: ServeOptions = {}): ServeTest {
    return serveTest(project, options);
}

/// One case on each of several variants of a project, named after it.
serve.each =
    (projects: readonly string[]) =>
    (name: string, body: ServeBody): void => {
        for (const project of projects) {
            serveTest(project, {})(`${name} [${project}]`, body);
        }
    };

/// Cases on a workspace of loose files, each source a unit: for files whose
/// content is what the case tests.
serve.files = (files: Record<string, FileText>, options: ServeOptions = {}): ServeTest =>
    serveTest(null, { ...options, files });
