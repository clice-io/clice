import * as os from "node:os";
import { defineConfig } from "vitest/config";

export default defineConfig({
    test: {
        include: ["integration/**/*.test.ts", "tools/**/*.test.ts"],
        pool: "forks",
        // Tests within one file run sequentially; files run in parallel, one
        // worker process per file — the same model as pytest-xdist's -n auto
        // (each test spawns a clice server: master + two workers, so the
        // worker count is the process-count throttle). Same-workspace
        // exclusivity across files is enforced by the session fixture's
        // per-workspace lock, not by the scheduler.
        fileParallelism: true,
        poolOptions: {
            forks: {
                maxForks: Math.max(1, os.availableParallelism()),
                minForks: 1,
            },
        },
        // A failing test's waits should burn ~2 minutes, not five: a large
        // default timeout amplifies every failure (and queues same-workspace
        // files behind the lock). The few legitimately slow tests override
        // per-test (crash recovery under ASan, stderr backpressure).
        testTimeout: 120_000,
        hookTimeout: 60_000,
        globalSetup: ["./integration/global_setup.ts"],
    },
});
