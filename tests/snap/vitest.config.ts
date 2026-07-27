import * as os from "node:os";
import { defineConfig } from "vitest/config";

/// The snap suite pins fixtures from both paths — `clice inspect`
/// processes (standalone) and real servers (wire) — so it is its own
/// vitest entry point, independent of the integration suite. Fixtures
/// within the single glue file run through test.concurrent, so
/// maxConcurrency — not file parallelism — is the throttle.
export default defineConfig({
    test: {
        include: ["snap/**/*.test.ts"],
        pool: "forks",
        maxConcurrency: Math.max(1, os.availableParallelism()),
        testTimeout: 120_000,
        hookTimeout: 120_000,
    },
});
