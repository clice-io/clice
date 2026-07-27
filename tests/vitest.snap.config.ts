import * as os from "node:os";
import { defineConfig } from "vitest/config";

/// The snap suite spawns `clice inspect` per corpus instead of a server;
/// it is its own vitest entry point so `npm test` (integration) and
/// `npm run snap` stay independently runnable.
export default defineConfig({
    test: {
        include: ["snap/**/*.test.ts"],
        pool: "forks",
        fileParallelism: true,
        poolOptions: {
            forks: {
                maxForks: Math.max(1, os.availableParallelism()),
                minForks: 1,
            },
        },
        testTimeout: 120_000,
        hookTimeout: 120_000,
    },
});
