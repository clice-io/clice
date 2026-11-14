/// Tests for the test client's initialization overlay (tools/client/client.ts).

import * as path from "node:path";
import { expect, test } from "vitest";
import { initializationOptionsFor } from "./client.ts";
import { Workspace } from "./workspace.ts";

const ws = new Workspace(path.join(path.sep, "ws"));

test("defaults overlay the caller's options", () => {
    const options = initializationOptionsFor(ws, {
        initializationOptions: { workers: { foreground: 3 } },
    });
    expect(options).toEqual({
        cache_dir: ws.path(".clice"),
        workers: { foreground: 3, background: 1, background_delay_ms: 0 },
        poll_seconds: 0,
    });
});

test("without test defaults only the cache is pinned", () => {
    // A perf run takes the server's real defaults: nothing but the cache
    // location is added, so the defaults stay spelled in the C++ config.
    const options = initializationOptionsFor(ws, {
        initializationOptions: { rules: [{ compile_commands: ["/cdb"] }] },
        testDefaults: false,
    });
    expect(options).toEqual({
        rules: [{ compile_commands: ["/cdb"] }],
        cache_dir: ws.path(".clice"),
    });
});

test("a cache of the caller's own", () => {
    const options = initializationOptionsFor(ws, { cacheDir: "/cache", testDefaults: false });
    expect(options).toEqual({ cache_dir: "/cache" });
});
