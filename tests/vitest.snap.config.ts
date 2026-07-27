import * as os from "node:os";
import { defineConfig } from "vitest/config";

/// The snap suite spawns `clice inspect` per fixture instead of a server;
/// it is its own vitest entry point so `npm test` (integration) and
/// `npm run snap` stay independently runnable. Fixtures within the single
/// glue file run through test.concurrent, so maxConcurrency — not file
/// parallelism — is the throttle.
export default defineConfig({
    test: {
        include: ["snap.test.ts"],
        maxConcurrency: Math.max(1, os.availableParallelism()),
        testTimeout: 120_000,
        hookTimeout: 120_000,
    },
});
