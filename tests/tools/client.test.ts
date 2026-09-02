/// Tests for the test client's initialization overlay (tools/client/client.ts).

import * as path from "node:path";
import { expect, test } from "vitest";
import { initializationOptionsFor } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";

const ws = new Workspace(path.join(path.sep, "ws"));

test("defaults overlay the caller's options", () => {
    const options = initializationOptionsFor(ws, {
        initializationOptions: { project: { stateful_worker_count: 3 }, tracker: {} },
    });
    expect(options).toEqual({
        project: {
            cache_dir: ws.path(".clice"),
            stateful_worker_count: 3,
            stateless_worker_count: 1,
        },
        tracker: { cdb_poll_seconds: 0, workspace_poll_seconds: 0 },
    });
});

test("without test defaults only the cache is pinned", () => {
    // A benchmark runs the server's real defaults: nothing but the cache
    // location is added, so the defaults stay spelled in the C++ config.
    const options = initializationOptionsFor(ws, {
        initializationOptions: { project: { compile_commands_paths: ["/cdb"] } },
        testDefaults: false,
    });
    expect(options).toEqual({
        project: { cache_dir: ws.path(".clice"), compile_commands_paths: ["/cdb"] },
        tracker: {},
    });
});
