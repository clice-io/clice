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
        // Match the old pytest --timeout=300; slow tests (modules, stress)
        // legitimately exceed one minute on loaded CI runners.
        testTimeout: 300_000,
        hookTimeout: 60_000,
        globalSetup: ["./global_setup.ts"],
    },
});
