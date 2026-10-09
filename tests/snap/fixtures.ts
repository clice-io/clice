/// vitest binding of the session machinery (tools/client/session.ts) for
/// the snap suite: the server driver replays fixtures through real servers,
/// so it needs the same `session` fixture and teardown gates as the
/// integration suite — bound independently, per suite.

import { test as base } from "vitest";
import { cliceExecutable, type SessionFactory } from "@clice/tools/session";
import { sessionFixture } from "../session_fixture.ts";

export { expect } from "vitest";
export { cliceExecutable };

export const test = base.extend<{ session: SessionFactory }>({
    session: sessionFixture,
});
